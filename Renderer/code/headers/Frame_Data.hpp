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
    // Only fields the shaders consume are here. The inverse matrices and
    // the clock values that used to travel in this block had no reader
    // on the GPU side and cost two matrix inversions per frame on the CPU
    // side; they were removed rather than uploaded unread.
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
        uint32_t         albedo_texture_index;   // INVALID_TEXTURE_INDEX = untextured
        uint32_t         albedo_sampler_index;   // slot in the bindless sampler array (a CoreTypes::Sampler_Preset value)
        uint32_t         _padding1;
        uint32_t         _padding2;
    };
    static_assert(sizeof(Push_Constants) == 96, "Push_Constants breaks the layout of push_constants.glsl");
    static_assert(offsetof(Push_Constants, base_color) == 64, "Push_Constants breaks the layout of push_constants.glsl");
    static_assert(offsetof(Push_Constants, albedo_texture_index) == 80, "Push_Constants breaks the layout of push_constants.glsl");
    static_assert(offsetof(Push_Constants, albedo_sampler_index) == 84, "Push_Constants breaks the layout of push_constants.glsl");

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
        device_handle(_other.device_handle),
        allocator(_other.allocator)
    {
        _other.image_available_semaphore = VK_NULL_HANDLE;
        _other.in_flight_fence = VK_NULL_HANDLE;
        _other.uniform_buffer = {};
        _other.light_buffer = {};
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
            device_handle = _other.device_handle;
            allocator = _other.allocator;

            _other.image_available_semaphore = VK_NULL_HANDLE;
            _other.in_flight_fence = VK_NULL_HANDLE;
            _other.uniform_buffer = {};
            _other.light_buffer = {};
            _other.device_handle = VK_NULL_HANDLE;
        }
        return *this;
    }

} // namespace Renderer_System
