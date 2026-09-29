#pragma once

#include <Renderer_Limits.hpp>

#include <Matrix.hpp>
#include <Vector.hpp>

#include <cstddef>
#include <cstdint>

namespace Renderer_System
{

    // =========================================================
    // GPU-side layouts: the C++ half of the C++/GLSL contract
    // =========================================================
    //
    // Each struct mirrors a block in Renderer/shaders/common/*.glsl. A
    // field added on one side and not the other shifts every following
    // offset without any warning, so both sides are edited together and
    // the static_asserts below pin the offsets the shaders rely on.
    //
    // Only Math types are used here: the layouts can be included, and
    // their asserts evaluated, without Vulkan. The layout of
    // VkDrawIndexedIndirectCommand, which the culling shader also
    // mirrors, is asserted in Frame_Data.hpp.

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

    // Mirror of `Draw_Bucket` in frame_set.glsl (std140, 16 bytes). One
    // bucket of the opaque draws: a range of the draw command buffer that
    // the culling pass fills for the objects whose Object_GPU::flags carry
    // the index of this bucket. Written every frame by the Renderer from
    // Draw_List_Builder::Get_opaque_buckets, as part of Frame_UBO.
    struct Draw_Bucket_GPU
    {
        uint32_t first_command;   // index of the bucket's first command in the draw command buffer
        uint32_t capacity;        // commands reserved: the objects assigned to the bucket
        uint32_t _padding0;
        uint32_t _padding1;
    };

    static_assert(sizeof(Draw_Bucket_GPU) == 16, "Draw_Bucket_GPU breaks the std140 layout of frame_set.glsl");

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
    //
    // The draw buckets of the GPU opaque paths close the block: the culling
    // pass reads there where each bucket starts in the draw command buffer
    // and how many commands it may hold.
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

        // Draw_Bucket draw_buckets[MAX_DRAW_BUCKETS]: only the buckets of
        // the frame are meaningful; the rest are zero.
        Draw_Bucket_GPU  draw_buckets[MAX_DRAW_BUCKETS];
    };

    static_assert(sizeof(Frame_UBO) == 336 + 16 * MAX_DRAW_BUCKETS, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, view_projection) == 128, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, camera_position) == 192, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, light_count) == 204, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, cluster_tiles_x) == 208, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, render_width) == 224, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, light_culling_mode) == 240, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, frustum_planes) == 256, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, draw_buckets) == 336, "Frame_UBO breaks the std140 layout of frame_set.glsl");

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

        // The upper 3x3 of the model matrix has a negative determinant
        // (a negative scale on an odd number of axes, or a reflection): the
        // transform inverts the winding of the triangles, so the object is
        // drawn with the opposite front face. Set by Draw_List_Builder for
        // every entry, and the reason the opaque objects are grouped by
        // (pipeline, winding). Shaders that use the sign of the tangent
        // frame (normal mapping) must flip the bitangent when it is set.
        inline constexpr uint32_t Mirrored = 1u << 9;

        // Bits 10-17: index of the draw bucket of an opaque object on the
        // GPU paths (Draw_Bucket_GPU); 0 elsewhere.
        inline constexpr uint32_t Bucket_Shift = 10;
        inline constexpr uint32_t Bucket_Mask = 0xFFu;

        // The flags bits of bucket _index.
        inline constexpr uint32_t Make_bucket_bits(uint32_t _index)
        {
            return (_index & Bucket_Mask) << Bucket_Shift;
        }
    }

    static_assert(MAX_DRAW_BUCKETS <= Object_Flag::Bucket_Mask + 1u, "The bucket index of an object must fit in Object_Flag::Bucket_Mask");

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
        uint32_t         flags;             // Object_Flag bits: pass mask (bits 0-7), Active (bit 8), Mirrored (bit 9), draw bucket (bits 10-17); bits 18-31 reserved
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
        uint32_t         alpha_mode;             // CoreTypes::Alpha_Mode value (MATERIAL_ALPHA_MODE_* in the shaders)
        float            alpha_cutoff;           // threshold of Alpha_Mode::Mask; 0 for the other modes
    };
    static_assert(sizeof(Material_GPU) == 32, "Material_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Material_GPU, albedo_texture_index) == 16, "Material_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Material_GPU, albedo_sampler_index) == 20, "Material_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Material_GPU, alpha_mode) == 24, "Material_GPU breaks the std430 layout of scene_data.glsl");
    static_assert(offsetof(Material_GPU, alpha_cutoff) == 28, "Material_GPU breaks the std430 layout of scene_data.glsl");

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

    // Mirror of `Draw_Count` in cull_objects.comp: the count buffers of
    // vkCmdDrawIndexedIndirectCount, one counter per draw bucket, and the
    // total. Reset to zero at the start of every frame, before the culling
    // pass. The draw of bucket b reads its count at
    // offsetof(Draw_Count_GPU, bucket_draw_count) + b * sizeof(uint32_t).
    struct Draw_Count_GPU
    {
        uint32_t total_draw_count;                        // commands written in all buckets: the objects the culling pass kept
        uint32_t _padding0;
        uint32_t _padding1;
        uint32_t _padding2;
        uint32_t bucket_draw_count[MAX_DRAW_BUCKETS];     // commands written in each bucket
    };
    static_assert(sizeof(Draw_Count_GPU) == 16 + 4 * MAX_DRAW_BUCKETS, "Draw_Count_GPU breaks the std430 layout of cull_objects.comp");
    static_assert(offsetof(Draw_Count_GPU, bucket_draw_count) == 16, "Draw_Count_GPU breaks the std430 layout of cull_objects.comp");

    // CPU-side copy of the GPU counters of one frame, filled by transfer
    // copies at the end of the compute work and read once the last
    // submission of the frame slot has completed (Renderer statistics).
    struct Frame_Stats_GPU
    {
        uint32_t cluster_light_references;   // Cluster_Counters_GPU::light_index_count
        uint32_t cluster_lights_dropped;     // Cluster_Counters_GPU::dropped_light_count
        uint32_t gpu_opaque_draws;           // Draw_Count_GPU::total_draw_count
        uint32_t _padding0;
    };
    static_assert(sizeof(Frame_Stats_GPU) == 16, "Frame_Stats_GPU layout changed: update Frame_Statistics::Record_readback");
    static_assert(offsetof(Frame_Stats_GPU, gpu_opaque_draws) == 8, "Frame_Stats_GPU layout changed: update Frame_Statistics::Record_readback");

} // namespace Renderer_System
