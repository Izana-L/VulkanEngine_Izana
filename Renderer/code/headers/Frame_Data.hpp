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
#include <cstddef>
#include <cstdint>

namespace Renderer_System
{

    // Capacity of the per-frame light buffer. The shader reads an
// unsized array, so raising this touches only the C++ side.
    static constexpr uint32_t MAX_LIGHTS = 16;

    // Capacity of the per-frame object buffer: one Object_GPU per draw,
    // opaque items first, then transparent ones. Draws beyond it are
    // skipped with a single warning. Growing a buffer that descriptors in
    // flight reference is out of scope; raise the constant instead.
    static constexpr uint32_t MAX_OBJECTS = 4096;

    // Capacity of the material table (set 2). Append-only: a slot, once
    // written, never changes. Slot 0 is CoreTypes::Default_Material.
    static constexpr uint32_t MAX_MATERIALS = 1024;


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

    // Mirror of `Frame_UBO` in frame_set.glsl (std140).
    //
    // The inverse matrices and the clock values that used to travel in
    // this block had no reader on the GPU side and cost two matrix
    // inversions per frame on the CPU side; they were removed rather than
    // uploaded unread. view and projection have no reader today either,
    // but cost only a copy: they stay for the passes that need the two
    // matrices apart (frame_set.glsl lists every field's consumer).
    struct Frame_UBO
    {
        MathLib::Matrix4 view;
        MathLib::Matrix4 projection;
        MathLib::Matrix4 view_projection;
        MathLib::Vector3 camera_position;   // world space, for view-dependent lighting terms
        int32_t          light_count;       // valid entries in the light buffer
    };

    static_assert(sizeof(Frame_UBO) == 208, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, view_projection) == 128, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, camera_position) == 192, "Frame_UBO breaks the std140 layout of frame_set.glsl");
    static_assert(offsetof(Frame_UBO, light_count) == 204, "Frame_UBO breaks the std140 layout of frame_set.glsl");

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
        uint32_t         mesh_index;        // Renderer mesh registry id; read by GPU culling (roadmap step 4)
        uint32_t         flags;             // bits 0-7: CoreTypes::Render_Pass_Bit of the item; bits 8-31 reserved
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
        // Uniform buffers
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

    private:

        VkDevice     device_handle;
        VmaAllocator allocator;

        void Destroy();
    };

    // ---------- Constructor ----------
    inline Frame_Data::Frame_Data(const Vulkan_Device& _device, VmaAllocator _allocator)
        : command_pool(_device, 1, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT),
        device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator)
    {
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
            uniform_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator, sizeof(Frame_UBO),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);

            light_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator, sizeof(Light_GPU) * MAX_LIGHTS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);

            object_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator, sizeof(Object_GPU) * MAX_OBJECTS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);
        
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

    inline void Frame_Data::Destroy()
    {
        if (device_handle == VK_NULL_HANDLE) return;

        Vulkan_Buffer_Utils::Destroy_buffer(allocator, object_buffer);
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, light_buffer);
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, uniform_buffer);

        if (in_flight_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_handle, in_flight_fence, nullptr);
            in_flight_fence = VK_NULL_HANDLE;
        }
        if (image_available_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_handle, image_available_semaphore, nullptr);
            image_available_semaphore = VK_NULL_HANDLE;
        }
    }

    inline Frame_Data::Frame_Data(Frame_Data&& _other) noexcept
        : command_pool(std::move(_other.command_pool)),
        image_available_semaphore(_other.image_available_semaphore),
        in_flight_fence(_other.in_flight_fence),
        uniform_buffer(_other.uniform_buffer),
        light_buffer(_other.light_buffer),
        object_buffer(_other.object_buffer),
        device_handle(_other.device_handle),
        allocator(_other.allocator)
    {
        _other.image_available_semaphore = VK_NULL_HANDLE;
        _other.in_flight_fence = VK_NULL_HANDLE;
        _other.uniform_buffer = {};
        _other.light_buffer = {};
        _other.object_buffer = {};
        _other.device_handle = VK_NULL_HANDLE;
    }

    inline Frame_Data& Frame_Data::operator=(Frame_Data&& _other) noexcept
    {
        if (this != &_other)
        {
            Destroy();

            command_pool = std::move(_other.command_pool);
            image_available_semaphore = _other.image_available_semaphore;
            in_flight_fence = _other.in_flight_fence;
            uniform_buffer = _other.uniform_buffer;
            light_buffer = _other.light_buffer;
            object_buffer = _other.object_buffer;
            device_handle = _other.device_handle;
            allocator = _other.allocator;

            _other.image_available_semaphore = VK_NULL_HANDLE;
            _other.in_flight_fence = VK_NULL_HANDLE;
            _other.uniform_buffer = {};
            _other.light_buffer = {};
            _other.object_buffer = {};
            _other.device_handle = VK_NULL_HANDLE;
        }
        return *this;
    }

} // namespace Renderer_System
