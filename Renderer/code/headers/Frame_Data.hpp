#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Command_Pool.hpp>
#include <Vulkan_Buffer_Utils.hpp>

#include <vk_mem_alloc.h>
#include <Vulkan_Utils.hpp>
#include <Matrix.hpp>
#include <Vector.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Renderer_System
{

    // Capacity of the per-frame light buffer. The shader reads an
// unsized array, so raising this touches only the C++ side.
    //
    // Directional lights are stored first, then the point and spot lights
    // the cluster pass distributes (Renderer::Write_frame_uniforms). A
    // packet with more lights keeps every directional light and drops the
    // local lights beyond the capacity.
    static constexpr uint32_t MAX_LIGHTS = 1024;

    // Capacity of the per-frame object buffer: one Object_GPU per draw,
    // opaque items first, then transparent ones. Draws beyond it are
    // skipped with a single warning. Growing a buffer that descriptors in
    // flight reference is out of scope; raise the constant instead.
    //
    // Also the capacity of the indirect command buffers: one command per
    // opaque object at most.
    static constexpr uint32_t MAX_OBJECTS = 16384;

    // Capacity of the material table (set 2). Append-only: a slot, once
    // written, never changes. Slot 0 is CoreTypes::Default_Material.
    static constexpr uint32_t MAX_MATERIALS = 1024;

    // Capacity of the mesh table (set 2): one Mesh_Info_GPU per mesh gpu
    // id. Gpu ids are never reused, so a released mesh keeps its slot.
    static constexpr uint32_t MAX_MESHES = 4096;

    // =========================================================
    // Clustered lighting
    // =========================================================
    //
    // The view frustum is split into CLUSTER_TILES_X x CLUSTER_TILES_Y
    // screen tiles and CLUSTER_SLICES depth slices. The tile count is
    // fixed, not the tile size in pixels, so no buffer depends on the
    // resolution and nothing is recreated on resize.
    //
    // Depth slices are exponential between the camera's near plane and
    // CLUSTER_MAX_DISTANCE (thinner close to the camera):
    //     slice k starts at  near * (CLUSTER_MAX_DISTANCE / near)^(k / CLUSTER_SLICES)
    // The projection has an infinite far plane, so the distribution needs
    // a finite end of its own. Fragments farther than the start of the
    // last slice use the last slice, whose volume reaches
    // CLUSTER_LAST_SLICE_FAR_DISTANCE: local lights farther than that do
    // not light anything.
    //
    // Cluster index = tile_x + tile_y * TILES_X + slice * TILES_X * TILES_Y,
    // with tile_y growing downwards like gl_FragCoord.y.
    static constexpr uint32_t CLUSTER_TILES_X = 16;
    static constexpr uint32_t CLUSTER_TILES_Y = 9;
    static constexpr uint32_t CLUSTER_SLICES = 24;
    static constexpr uint32_t CLUSTER_COUNT = CLUSTER_TILES_X * CLUSTER_TILES_Y * CLUSTER_SLICES;

    static constexpr float    CLUSTER_MAX_DISTANCE = 200.0f;
    static constexpr float    CLUSTER_LAST_SLICE_FAR_DISTANCE = 1.0e5f;

    // Capacity of the compacted light index list, sized for an average of
    // CLUSTER_AVERAGE_LIGHTS lights per cluster (4 bytes per entry, one
    // list per frame in flight). When a frame needs more, the clusters that
    // do not fit keep part of their lights and the loss is counted in
    // Cluster_Counters_GPU::dropped_light_count.
    static constexpr uint32_t CLUSTER_AVERAGE_LIGHTS = 64;
    static constexpr uint32_t CLUSTER_LIGHT_INDEX_CAPACITY = CLUSTER_COUNT * CLUSTER_AVERAGE_LIGHTS;

    // =========================================================
    // Compute push constants
    // =========================================================

    // Size of the push constant range of the compute pipeline layout,
    // shared by every compute pipeline. Each compute shader declares its
    // own block of at most this size at offset 0.
    static constexpr uint32_t COMPUTE_PUSH_CONSTANT_SIZE = 16;


    // =========================================================
    // GPU-side layouts: the C++ half of the C++/GLSL contract
    // =========================================================
    //
    // Each struct mirrors a block in Renderer/shaders/common/*.glsl. A
    // field added on one side and not the other shifts every following
    // offset without any warning, so both sides are edited together and
    // the static_asserts below pin the offsets the shaders rely on.

    // Mirror of `Light` in frame_set.glsl (std430).
    struct Light_GPU
    {
        MathLib::Vector3 position_or_direction;
        float            intensity;
        MathLib::Vector3 color;
        float            range;
        MathLib::Vector3 spot_direction;
        float            inner_angle;
        float            outer_angle;
        int32_t          type;            // 0=directional, 1=point, 2=spot
        float            _padding0;
        float            _padding1;
    };

    static_assert(sizeof(Light_GPU) == 64, "Light_GPU breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(Light_GPU, color) == 16, "Light_GPU breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(Light_GPU, spot_direction) == 32, "Light_GPU breaks the std430 layout of frame_set.glsl");

    // How mesh.frag selects the lights of a fragment. Mirrored by the
    // LIGHT_CULLING_* constants of frame_set.glsl.
    //   Clustered   - directional lights, then the list of the fragment's
    //                 cluster.
    //   Brute_Force - every light of the buffer: the reference the
    //                 clustered path must match.
    enum class Light_Culling_Mode : uint32_t
    {
        Clustered = 0,
        Brute_Force = 1,
        Count
    };

    // Debug views of the cluster grid, drawn by mesh.frag instead of the
    // lit color. Mirrored by the CLUSTER_VIEW_* constants of frame_set.glsl.
    //   None          - normal shading;
    //   Light_Heatmap - number of lights of the fragment's cluster
    //                   (blue = few, red = Frame_UBO::heatmap_max_lights or
    //                   more, magenta = cluster that lost lights to a full
    //                   index list);
    //   Depth_Slices  - one color per depth slice: bands that get thinner
    //                   towards the camera;
    //   Clusters      - one color per cluster: the tile grid on screen
    //                   combined with the slices.
    enum class Cluster_Debug_View : uint32_t
    {
        None = 0,
        Light_Heatmap = 1,
        Depth_Slices = 2,
        Clusters = 3,
        Count
    };

    // Planes of the culling frustum carried by Frame_UBO; the same count as
    // CoreTypes::Frustum (left, right, bottom, top, near; no far plane).
    static constexpr uint32_t FRUSTUM_PLANE_COUNT = 5;

    // Mirror of `Frame_UBO` in frame_set.glsl (std140).
    //
    // The inverse matrices and the clock values that used to travel in
    // this block had no reader on the GPU side and cost two matrix
    // inversions per frame on the CPU side; they were removed rather than
    // uploaded unread. view and projection have no reader today either,
    // but cost only a copy: they stay for the passes that need the two
    // matrices apart (frame_set.glsl lists every field's consumer).
    //
    // The scalars after light_count are grouped four by four, so every
    // group sits on a 16-byte boundary as std140 requires for a uvec4 /
    // vec4, and the planes form an array of vec4 (std140 stride 16).
    struct Frame_UBO
    {
        MathLib::Matrix4 view;
        MathLib::Matrix4 projection;
        MathLib::Matrix4 view_projection;
        MathLib::Vector3 camera_position;   // world space, for view-dependent lighting terms
        int32_t          light_count;       // valid entries in the light buffer, directional ones first

        // uvec4 cluster_grid
        uint32_t         cluster_tiles_x;
        uint32_t         cluster_tiles_y;
        uint32_t         cluster_slices;
        uint32_t         directional_light_count;   // lights [0, count) are directional; the rest are clustered

        // vec4 cluster_params
        float            render_width;          // extent of the frame in pixels: maps gl_FragCoord to a tile
        float            render_height;
        float            cluster_slice_scale;   // slice = floor(log(view_depth) * scale + bias)
        float            cluster_slice_bias;

        // uvec4 debug_params
        uint32_t         light_culling_mode;    // Light_Culling_Mode
        uint32_t         cluster_debug_view;    // Cluster_Debug_View
        uint32_t         heatmap_max_lights;    // light count drawn as full red by the heatmap
        uint32_t         _padding0;

        // vec4 frustum_planes[FRUSTUM_PLANE_COUNT]: world space, xyz = unit
        // normal pointing inside, w = offset; a point p is inside a plane
        // when dot(xyz, p) + w >= 0. Read by the culling pass.
        MathLib::Vector4 frustum_planes[FRUSTUM_PLANE_COUNT];
    };

    static_assert(sizeof(Frame_UBO) == 336, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, view_projection) == 128, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, camera_position) == 192, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, light_count) == 204, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, cluster_tiles_x) == 208, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, render_width) == 224, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, light_culling_mode) == 240, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, frustum_planes) == 256, "Frame_UBO breaks the std140 layout of frame_set.glsl");

    // Mirror of `Push_Constants` in push_constants.glsl. One block for
    // both stages: the vertex shader reads `model`, the fragment shader
    // reads `base_color`, `albedo_texture_index` and
    // `albedo_sampler_index`. 96 bytes, inside the 128-byte minimum every
    // Vulkan implementation guarantees.
    struct Push_Constants
    {
        MathLib::Matrix4 model;
        MathLib::Vector4 base_color;
        uint32_t         albedo_texture_index;   // bindless texture slot; untextured = CoreTypes::Default_Texture::White
        uint32_t         albedo_sampler_index;   // slot in the bindless sampler array (a CoreTypes::Sampler_Preset value)
        uint32_t         _padding1;
        uint32_t         _padding2;
    };
    static_assert(sizeof(Push_Constants) == 96, "Push_Constants breaks the layout of push_constants.glsl");
    static_assert(offsetof(Push_Constants, base_color) == 64, "Push_Constants breaks the layout of push_constants.glsl");
    static_assert(offsetof(Push_Constants, albedo_texture_index) == 80, "Push_Constants breaks the layout of push_constants.glsl");
    static_assert(offsetof(Push_Constants, albedo_sampler_index) == 84, "Push_Constants breaks the layout of push_constants.glsl");

    // Mirror of `Procedural_Push_Constants` in procedural.comp. Compute
    // stage only: declared in the compute pipeline layout, never in the
    // graphics one. 16 bytes.
    struct Procedural_Push_Constants
    {
        uint32_t image_width;    // texels; the shader reads both as a uvec2
        uint32_t image_height;
        float    time;           // seconds since start, drives the animation
        float    _padding0;
    };
    static_assert(sizeof(Procedural_Push_Constants) == 16, "Procedural_Push_Constants breaks the layout of procedural.comp");
    static_assert(offsetof(Procedural_Push_Constants, image_width) == 0, "Procedural_Push_Constants breaks the layout of procedural.comp");
    static_assert(offsetof(Procedural_Push_Constants, time) == 8, "Procedural_Push_Constants breaks the layout of procedural.comp");
    static_assert(sizeof(Procedural_Push_Constants) <= COMPUTE_PUSH_CONSTANT_SIZE, "Procedural_Push_Constants exceeds the compute push constant range");

    // Mirror of `Cluster_Push_Constants` in cluster_lights.comp. 16 bytes.
    // The pass tests lights [first_local_light, light_count): the
    // directional lights at the start of the buffer are not clustered.
    struct Cluster_Push_Constants
    {
        uint32_t cluster_count;           // invocations beyond it return at once
        uint32_t light_index_capacity;    // entries of the light index list
        uint32_t first_local_light;       // = Frame_UBO::directional_light_count
        uint32_t light_count;             // = Frame_UBO::light_count
    };
    static_assert(sizeof(Cluster_Push_Constants) == 16, "Cluster_Push_Constants breaks the layout of cluster_lights.comp");
    static_assert(sizeof(Cluster_Push_Constants) <= COMPUTE_PUSH_CONSTANT_SIZE, "Cluster_Push_Constants exceeds the compute push constant range");

    // Mirror of `Cull_Push_Constants` in cull_objects.comp. 16 bytes.
    // The pass reads objects [0, object_count): the opaque objects, which
    // the Renderer writes first in the object buffer.
    struct Cull_Push_Constants
    {
        uint32_t object_count;       // invocations beyond it return at once
        uint32_t command_capacity;   // entries of the draw command buffer
        uint32_t pass_bit;           // CoreTypes::Render_Pass_Bit an object needs to be drawn
        uint32_t frustum_culling;    // 0: every active object gets a command (compaction only)
    };
    static_assert(sizeof(Cull_Push_Constants) == 16, "Cull_Push_Constants breaks the layout of cull_objects.comp");
    static_assert(sizeof(Cull_Push_Constants) <= COMPUTE_PUSH_CONSTANT_SIZE, "Cull_Push_Constants exceeds the compute push constant range");

    // Bits of Object_GPU::flags. Mirrored by the OBJECT_FLAG_* constants of
    // scene_data.glsl.
    namespace Object_Flag
    {
        // Bits 0-7: the CoreTypes::Render_Pass_Bit mask of the draw item.
        inline constexpr uint32_t Pass_Mask = 0xFFu;

        // The entry describes an object to draw this frame. The culling
        // pass skips entries without it, so an object can be disabled on
        // the GPU without compacting the buffer.
        inline constexpr uint32_t Active = 1u << 8;
    }

    // Mirror of `Object` in scene_data.glsl (std430). One entry per draw in
    // the per-frame object buffer (set 0, Binding_Per_Frame::Objects); the
    // draw passes its index as firstInstance and the vertex shader reads
    // objects[gl_InstanceIndex].
    //
    // normal_matrix is a mat4 on purpose: a mat3 occupies three vec4
    // columns in std430 (48 bytes, not 36), so a mat4 costs 16 bytes more
    // and removes any padding mismatch. The shader uses its upper 3x3.
    struct Object_GPU
    {
        MathLib::Matrix4 model;             // world matrix
        MathLib::Matrix4 normal_matrix;     // transpose(inverse(model)), computed on the CPU
        uint32_t         material_index;    // slot in the material table (set 2)
        uint32_t         mesh_index;        // Renderer mesh registry id = slot in the mesh table (set 2)
        uint32_t         flags;             // Object_Flag bits: pass mask (bits 0-7), Active (bit 8); bits 9-31 reserved
        uint32_t         _padding0;         // keeps the stride at a multiple of 16
    };
    static_assert(sizeof(Object_GPU) == 144, "Object_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Object_GPU, normal_matrix) == 64, "Object_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Object_GPU, material_index) == 128, "Object_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Object_GPU, flags) == 136, "Object_GPU breaks the std430 layout of scene_data.glsl");

    // Mirror of `Material` in scene_data.glsl (std430). One entry per
    // registered material in the material table (set 2,
    // Binding_Per_Material::Materials); objects reference it through
    // Object_GPU::material_index.
    struct Material_GPU
    {
        MathLib::Vector4 base_color;             // tint multiplied with vertex color and albedo sample
        uint32_t         albedo_texture_index;   // bindless texture slot; untextured = CoreTypes::Default_Texture::White
        uint32_t         albedo_sampler_index;   // slot in the bindless sampler array (a CoreTypes::Sampler_Preset value)
        uint32_t         _padding0;
        uint32_t         _padding1;
    };
    static_assert(sizeof(Material_GPU) == 32, "Material_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Material_GPU, albedo_texture_index) == 16, "Material_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Material_GPU, albedo_sampler_index) == 20, "Material_GPU breaks the std430 layout of scene_data.glsl");

    // Mirror of `Mesh_Info` in mesh_table.glsl (std430). One entry per mesh
    // gpu id in the mesh table (set 2, Binding_Per_Material::Meshes),
    // written at upload: where the mesh sits in the Geometry_Pool, in draw
    // units, and its bounding sphere in mesh space.
    struct Mesh_Info_GPU
    {
        MathLib::Vector4 bounding_sphere;   // xyz = center, w = radius (mesh space)
        uint32_t         first_index;       // firstIndex of the draw
        uint32_t         index_count;
        int32_t          vertex_offset;     // vertexOffset of the draw
        uint32_t         vertex_count;
    };
    static_assert(sizeof(Mesh_Info_GPU) == 32, "Mesh_Info_GPU breaks the std430 layout of mesh_table.glsl");
    static_assert(offsetof(Mesh_Info_GPU, first_index) == 16, "Mesh_Info_GPU breaks the std430 layout of mesh_table.glsl");
    static_assert(offsetof(Mesh_Info_GPU, vertex_offset) == 24, "Mesh_Info_GPU breaks the std430 layout of mesh_table.glsl");

    // Mirror of `Cluster_AABB` in cluster_data.glsl (std430): the view
    // space box of one cluster (w unused). Built on the CPU
    // (Cluster_Grid::Build_aabbs) whenever the projection changes.
    struct Cluster_AABB_GPU
    {
        MathLib::Vector4 min_point;
        MathLib::Vector4 max_point;
    };
    static_assert(sizeof(Cluster_AABB_GPU) == 32, "Cluster_AABB_GPU breaks the std430 layout of cluster_data.glsl");

    // Mirror of one `uvec2` of the cluster grid in cluster_data.glsl: the
    // lights of the cluster are light_indices[offset, offset + count).
    // Bit 31 of count (CLUSTER_OVERFLOW_BIT) marks a cluster that lost
    // lights because the index list was full; the count itself is in bits
    // 0-30.
    struct Cluster_Range_GPU
    {
        uint32_t offset;
        uint32_t count;
    };
    static_assert(sizeof(Cluster_Range_GPU) == 8, "Cluster_Range_GPU breaks the std430 layout of cluster_data.glsl");

    // Mirror of `Cluster_Counters` in cluster_data.glsl. Reset to zero at
    // the start of every frame, before the cluster pass.
    struct Cluster_Counters_GPU
    {
        uint32_t light_index_count;     // entries requested this frame (may exceed the capacity)
        uint32_t dropped_light_count;   // entries that did not fit in the list
        uint32_t _padding0;
        uint32_t _padding1;
    };
    static_assert(sizeof(Cluster_Counters_GPU) == 16, "Cluster_Counters_GPU breaks the std430 layout of cluster_data.glsl");

    // Mirror of `Draw_Count` in cull_objects.comp: the count buffer of
    // vkCmdDrawIndexedIndirectCount. Reset to zero at the start of every
    // frame, before the culling pass.
    struct Draw_Count_GPU
    {
        uint32_t draw_count;
        uint32_t _padding0;
        uint32_t _padding1;
        uint32_t _padding2;
    };
    static_assert(sizeof(Draw_Count_GPU) == 16, "Draw_Count_GPU breaks the std430 layout of cull_objects.comp");

    // The indirect command written by the culling pass and read by the
    // draw: `Draw_Command` in cull_objects.comp mirrors it (five 32-bit
    // values, std430 stride 20).
    static_assert(sizeof(VkDrawIndexedIndirectCommand) == 20, "Draw_Command in cull_objects.comp assumes a 20-byte VkDrawIndexedIndirectCommand");

    // CPU-side copy of the GPU counters of one frame, filled by transfer
    // copies at the end of the compute work and read after the fence of
    // the frame slot (Renderer statistics).
    struct Frame_Stats_GPU
    {
        uint32_t cluster_light_references;   // Cluster_Counters_GPU::light_index_count
        uint32_t cluster_lights_dropped;     // Cluster_Counters_GPU::dropped_light_count
        uint32_t gpu_opaque_draws;           // Draw_Count_GPU::draw_count
        uint32_t _padding0;
    };
    static_assert(sizeof(Frame_Stats_GPU) == 16, "Frame_Stats_GPU layout changed: update the copies in Record_command_buffer");
    static_assert(offsetof(Frame_Stats_GPU, gpu_opaque_draws) == 8, "Frame_Stats_GPU layout changed: update the copies in Record_command_buffer");

    // =========================================================
    // Frame_Data
    // =========================================================

    // Frame_Data: the Vulkan resources that must exist independently for
    // each frame-in-flight slot.
    //
    // With FRAMES_IN_FLIGHT = 2, two Frame_Data instances exist:
    // one for the frame the CPU is currently recording, and one for the
    // frame the GPU is currently executing. This overlap is what gives
    // CPU/GPU parallelism without stalling either side.
    //
    // What is NOT here: the semaphore the present operation waits on
    // (render finished) and the present fence. Those belong to the
    // SWAPCHAIN IMAGE, not to the frame slot: a present may still be
    // waiting on them when the slot comes around again, and there are
    // more images than slots. Renderer keeps them per image.
    //
    // Lifetime: owned by Renderer, constructed once at startup,
    // destroyed at shutdown. Move-only (owns Vulkan handles).
    //
    // The uniform buffers are mapped persistently at construction time
    // (VMA_ALLOCATION_CREATE_MAPPED_BIT). The Renderer writes into
    // uniform_buffer.mapped_ptr directly each frame with std::memcpy - no
    // map/unmap overhead per frame.
    //
    // Buffers written by the GPU (cluster grid and list, counters, GPU draw
    // commands) are per slot as well: a frame then never writes what the
    // previous frame, possibly still executing, reads, and the fence of the
    // slot is the only ordering needed against the frame that used the
    // same copy before.
    class Frame_Data
    {
    public:

        Frame_Data(const Vulkan_Device& _device, VmaAllocator _allocator);
        ~Frame_Data();

        Frame_Data(const Frame_Data&) = delete;
        Frame_Data& operator=(const Frame_Data&) = delete;

        Frame_Data(Frame_Data&& _other) noexcept;
        Frame_Data& operator=(Frame_Data&& _other) noexcept;

        // =========================================================
        // Command recording
        // =========================================================

        // One pool per slot so resetting this slot's buffer never touches
        // the buffer the GPU is still executing for the other slot.
        Vulkan_Command_Pool command_pool;

        VkCommandBuffer Get_command_buffer() const
        {
            return command_pool.Get_command_buffer(0);
        }

        // =========================================================
        // Synchronization
        // =========================================================

        // Signaled by the swapchain when the acquired image is ready to be
        // written to. The GPU waits on this before the color attachment
        // output stage.
        VkSemaphore image_available_semaphore = VK_NULL_HANDLE;

        // Signaled by the GPU when this frame's commands are done.
        // The CPU waits on this at the start of the next use of this
        // slot to ensure the GPU has finished with these resources.
        // Created pre-signaled so the first wait returns immediately.
        VkFence in_flight_fence = VK_NULL_HANDLE;

        // =========================================================
        // Buffers written by the CPU (host-visible, persistently mapped)
        // =========================================================

        // Buffer, allocation and the persistent CPU-side pointer, all
        // in one. uniform_buffer.mapped_ptr is valid for the entire
        // lifetime of this Frame_Data.
        Vulkan_Buffer_Utils::Buffer_Allocation uniform_buffer;

        // MAX_LIGHTS entries of Light_GPU, storage buffer.
        Vulkan_Buffer_Utils::Buffer_Allocation light_buffer;

        // MAX_OBJECTS entries of Object_GPU, storage buffer, persistently
        // mapped. Rewritten entirely every frame by the Renderer, one entry
        // per draw; the entry index is the draw's firstInstance.
        Vulkan_Buffer_Utils::Buffer_Allocation object_buffer;

        // MAX_OBJECTS VkDrawIndexedIndirectCommand, INDIRECT_BUFFER usage:
        // the opaque draws when the CPU builds them
        // (Opaque_Draw_Path::Cpu_Indirect), contiguous per pipeline.
        Vulkan_Buffer_Utils::Buffer_Allocation cpu_draw_command_buffer;

        // =========================================================
        // Buffers written by the GPU (device-local)
        // =========================================================

        // CLUSTER_COUNT Cluster_Range_GPU, written by cluster_lights.comp,
        // read by mesh.frag.
        Vulkan_Buffer_Utils::Buffer_Allocation cluster_grid_buffer;

        // CLUSTER_LIGHT_INDEX_CAPACITY light indices, compacted per cluster.
        Vulkan_Buffer_Utils::Buffer_Allocation cluster_light_index_buffer;

        // One Cluster_Counters_GPU: zero-filled (TRANSFER_DST), incremented
        // with atomics, copied for statistics (TRANSFER_SRC).
        Vulkan_Buffer_Utils::Buffer_Allocation cluster_counter_buffer;

        // MAX_OBJECTS VkDrawIndexedIndirectCommand written by
        // cull_objects.comp and consumed by vkCmdDrawIndexedIndirectCount
        // (STORAGE_BUFFER | INDIRECT_BUFFER).
        Vulkan_Buffer_Utils::Buffer_Allocation gpu_draw_command_buffer;

        // One Draw_Count_GPU: zero-filled, incremented by the culling pass,
        // read as the count buffer of the indirect draw and copied for
        // statistics.
        Vulkan_Buffer_Utils::Buffer_Allocation gpu_draw_count_buffer;

        // =========================================================
        // Readback
        // =========================================================

        // One Frame_Stats_GPU, host-visible (Gpu_To_Cpu), filled by transfer
        // copies of the counters and read after the fence of the slot.
        Vulkan_Buffer_Utils::Buffer_Allocation stats_readback_buffer;

    private:

        VkDevice     device_handle;
        VmaAllocator allocator;

        // Every buffer member in one list, so destruction and moves cannot
        // forget one. A new buffer member is added here and BUFFER_COUNT
        // raised.
        static constexpr size_t BUFFER_COUNT = 10;
        std::array<Vulkan_Buffer_Utils::Buffer_Allocation*, BUFFER_COUNT> All_buffers();

        void Destroy();
        void Take_from(Frame_Data& _other) noexcept;
    };

    // ---------- Constructor ----------
    inline Frame_Data::Frame_Data(const Vulkan_Device& _device, VmaAllocator _allocator)
        : command_pool(_device, 1, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT),
        device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator)
    {
        using Vulkan_Buffer_Utils::Create_buffer;
        using Vulkan_Buffer_Utils::Buffer_Access;

        // Anything that throws below leaves the already created handles to
        // Destroy(), called from the destructor of a partially built
        // object: handles start null, so nothing is destroyed twice.
        try
        {
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            VK_CHECK(vkCreateSemaphore(device_handle, &semaphore_info, nullptr, &image_available_semaphore),
                "Frame_Data: failed to create image_available semaphore");

            // in_flight_fence is pre-signaled so the first vkWaitForFences
            // on this slot returns immediately - there's nothing in flight yet.
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            VK_CHECK(vkCreateFence(device_handle, &fence_info, nullptr, &in_flight_fence),
                "Frame_Data: failed to create in_flight fence");

            // Persistently mapped, written each frame with std::memcpy.
            uniform_buffer = Create_buffer(allocator, sizeof(Frame_UBO),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Buffer_Access::Cpu_To_Gpu, true);

            light_buffer = Create_buffer(allocator, sizeof(Light_GPU) * MAX_LIGHTS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer_Access::Cpu_To_Gpu, true);

            object_buffer = Create_buffer(allocator, sizeof(Object_GPU) * MAX_OBJECTS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer_Access::Cpu_To_Gpu, true);

            cpu_draw_command_buffer = Create_buffer(allocator, sizeof(VkDrawIndexedIndirectCommand) * MAX_OBJECTS,
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, Buffer_Access::Cpu_To_Gpu, true);

            cluster_grid_buffer = Create_buffer(allocator, sizeof(Cluster_Range_GPU) * CLUSTER_COUNT,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer_Access::Gpu_Only);

            cluster_light_index_buffer = Create_buffer(allocator, sizeof(uint32_t) * CLUSTER_LIGHT_INDEX_CAPACITY,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer_Access::Gpu_Only);

            cluster_counter_buffer = Create_buffer(allocator, sizeof(Cluster_Counters_GPU),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                Buffer_Access::Gpu_Only);

            gpu_draw_command_buffer = Create_buffer(allocator, sizeof(VkDrawIndexedIndirectCommand) * MAX_OBJECTS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, Buffer_Access::Gpu_Only);

            gpu_draw_count_buffer = Create_buffer(allocator, sizeof(Draw_Count_GPU),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                Buffer_Access::Gpu_Only);

            stats_readback_buffer = Create_buffer(allocator, sizeof(Frame_Stats_GPU),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer_Access::Gpu_To_Cpu, true);
        }
        catch (...)
        {
            Destroy();
            throw;
        }
    }

    inline Frame_Data::~Frame_Data()
    {
        Destroy();
    }

    inline std::array<Vulkan_Buffer_Utils::Buffer_Allocation*, Frame_Data::BUFFER_COUNT> Frame_Data::All_buffers()
    {
        return { &uniform_buffer, &light_buffer, &object_buffer, &cpu_draw_command_buffer,
                 &cluster_grid_buffer, &cluster_light_index_buffer, &cluster_counter_buffer,
                 &gpu_draw_command_buffer, &gpu_draw_count_buffer, &stats_readback_buffer };
    }

    inline void Frame_Data::Destroy()
    {
        if (device_handle == VK_NULL_HANDLE) return;

        for (Vulkan_Buffer_Utils::Buffer_Allocation* buffer : All_buffers())
            Vulkan_Buffer_Utils::Destroy_buffer(allocator, *buffer);

        if (in_flight_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_handle, in_flight_fence, nullptr);
            in_flight_fence = VK_NULL_HANDLE;
        }
        if (image_available_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_handle, image_available_semaphore, nullptr);
            image_available_semaphore = VK_NULL_HANDLE;
        }
    }

    // Steals every handle of _other and leaves it empty (its Destroy() then
    // does nothing). The command pool is moved by the callers.
    inline void Frame_Data::Take_from(Frame_Data& _other) noexcept
    {
        image_available_semaphore = _other.image_available_semaphore;
        in_flight_fence = _other.in_flight_fence;
        device_handle = _other.device_handle;
        allocator = _other.allocator;

        const auto destination = All_buffers();
        const auto source = _other.All_buffers();

        for (size_t i = 0; i < BUFFER_COUNT; ++i)
        {
            *destination[i] = *source[i];
            *source[i] = {};
        }

        _other.image_available_semaphore = VK_NULL_HANDLE;
        _other.in_flight_fence = VK_NULL_HANDLE;
        _other.device_handle = VK_NULL_HANDLE;
    }

    inline Frame_Data::Frame_Data(Frame_Data&& _other) noexcept
        : command_pool(std::move(_other.command_pool)),
        device_handle(VK_NULL_HANDLE),
        allocator(VK_NULL_HANDLE)
    {
        Take_from(_other);
    }

    inline Frame_Data& Frame_Data::operator=(Frame_Data&& _other) noexcept
    {
        if (this != &_other)
        {
            Destroy();

            command_pool = std::move(_other.command_pool);
            Take_from(_other);
        }
        return *this;
    }

} // namespace Renderer_System
