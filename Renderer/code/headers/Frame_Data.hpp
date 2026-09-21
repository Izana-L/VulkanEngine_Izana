#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Buffer_Utils.hpp>

#include <vk_mem_alloc.h>
#include <Vulkan_Utils.hpp>
#include <Matrix.hpp>
#include <Vector.hpp>
#include <cassert>
#include <cstdint>
#include <stdexcept>

namespace Renderer_System
{

    
    static constexpr uint32_t MAX_LIGHTS = 16;

    
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

    static_assert(sizeof(Light_GPU) == 64, "Light_GPU rompe std430");
    static_assert(offsetof(Light_GPU, color) == 16, "Light_GPU rompe std430");
    static_assert(offsetof(Light_GPU, spot_direction) == 32, "Light_GPU rompe std430");

    
    struct Frame_UBO
    {
        MathLib::Matrix4 view;
        MathLib::Matrix4 projection;
        MathLib::Matrix4 view_projection;
        MathLib::Matrix4 inv_view;          
        MathLib::Matrix4 inv_projection;   
        MathLib::Vector3 camera_position;
        int32_t          light_count;       
        float            time;              
        float            delta_time;
        float            _padding0;
        float            _padding1;
    };

    static_assert(sizeof(Frame_UBO) == 352, "Frame_UBO rompe std140");
    static_assert(offsetof(Frame_UBO, inv_view) == 192, "Frame_UBO rompe std140");
    static_assert(offsetof(Frame_UBO, camera_position) == 320, "Frame_UBO rompe std140");
    static_assert(offsetof(Frame_UBO, time) == 336, "Frame_UBO rompe std140");
    // Frame_Data: all Vulkan resources that must exist independently for
    // each frame-in-flight slot.
    //
    // With FRAMES_IN_FLIGHT = 2, two Frame_Data instances exist:
    // one for the frame the CPU is currently recording, and one for the
    // frame the GPU is currently executing. This overlap is what gives
    // us CPU/GPU parallelism without stalling either side.
    //
    // Lifetime: owned by Renderer, constructed once at startup,
    // destroyed at shutdown. Not copyable — owns Vulkan handles.
    //
    // The uniform buffer is mapped persistently at construction time
    // (VMA_ALLOCATION_CREATE_MAPPED_BIT). The Renderer writes into
    // uniform_buffer.mapped_ptr directly each frame with std::memcpy — no
    // map/unmap overhead per frame.
    struct Frame_Data
    {
        // =========================================================
        // Command recording
        // =========================================================

        VkCommandPool   command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;

        // =========================================================
        // Synchronization
        // =========================================================

        // Signaled by the swapchain when the image at this slot is
        // ready to be written to. The GPU waits on this before the
        // color attachment output stage.
        VkSemaphore image_available_semaphore = VK_NULL_HANDLE;

        // Signaled by the GPU when rendering is complete.
        // The swapchain waits on this before presenting.
        VkSemaphore render_finished_semaphore = VK_NULL_HANDLE;

        // Signaled by the GPU when this frame's commands are done.
        // The CPU waits on this at the start of the next use of this
        // slot to ensure the GPU has finished with these resources.
        // Created pre-signaled so the first wait returns immediately.
        VkFence in_flight_fence = VK_NULL_HANDLE;

        // Signaled by the presentation engine when the present operation
        // for this slot completes (VK_KHR_swapchain_maintenance1).
        // Waited on before reusing render_finished_semaphore, which is the
        // semaphore the present operation waits on — without this, the
        // semaphore could be reused while still pending in a present.
        // NOT pre-signaled: the first frame has no prior present to wait on,
        // so present_fence_pending tracks whether it's been used yet.
        VkFence present_fence = VK_NULL_HANDLE;

        // True once present_fence has been submitted to a present at least
        // once. Guards the first-frame case where there's nothing to wait on.
        bool present_fence_pending = false;

        // =========================================================
        // Uniform buffer
        // =========================================================

        // Buffer, allocation and the persistent CPU-side pointer, all
        // in one. uniform_buffer.mapped_ptr is valid for the entire
        // lifetime of this Frame_Data.
        // Write here each frame:
        //   std::memcpy(uniform_buffer.mapped_ptr, &ubo, sizeof(ubo))
        Vulkan_Buffer_Utils::Buffer_Allocation uniform_buffer;

        Vulkan_Buffer_Utils::Buffer_Allocation light_buffer;
        // =========================================================
        // Lifecycle
        // =========================================================

        // Allocates all resources for this frame slot.
        void Init(const Vulkan_Device& _device, VmaAllocator _allocator)
        {
            VkDevice device = _device.Get_logical_device_handle();

            // ── Command pool + buffer ──────────────────────────────
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = _device.Get_queue_family_indices().graphics_family.value();

            VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Frame_Data: failed to create command pool: " +
                    Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            VkCommandBufferAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc_info.commandPool = command_pool;
            alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc_info.commandBufferCount = 1;

            result = vkAllocateCommandBuffers(device, &alloc_info, &command_buffer);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Frame_Data: failed to allocate command buffer: " +
                    Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            // ── Semaphores ─────────────────────────────────────────
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            result = vkCreateSemaphore(device, &semaphore_info, nullptr, &image_available_semaphore);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Frame_Data: failed to create image_available semaphore: " +
                    Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            result = vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished_semaphore);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Frame_Data: failed to create render_finished semaphore: " +
                    Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            // ── Fences ─────────────────────────────────────────────
            // in_flight_fence is pre-signaled so the first vkWaitForFences
            // on frame 0 returns immediately — there's nothing in flight yet.
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            result = vkCreateFence(device, &fence_info, nullptr, &in_flight_fence);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Frame_Data: failed to create in_flight fence: " +
                    Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            // present_fence is NOT pre-signaled — it's only signaled after
            // a real present operation. present_fence_pending guards the
            // first-frame case.
            VkFenceCreateInfo present_fence_info{};
            present_fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            present_fence_info.flags = 0;

            result = vkCreateFence(device, &present_fence_info, nullptr, &present_fence);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Frame_Data: failed to create present fence: " +
                    Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            // ── Uniform buffer (persistently mapped) ──────────────
            // Mapped once at creation and kept mapped for the lifetime of
            // this frame slot.
            uniform_buffer = Vulkan_Buffer_Utils::Create_buffer(_allocator, sizeof(Frame_UBO),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 
                                                                Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu,true);
            light_buffer = Vulkan_Buffer_Utils::Create_buffer(_allocator,sizeof(Light_GPU) * MAX_LIGHTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                              Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu,true);
        }

        // Destroys all resources owned by this frame slot.
        // Safe to call on a default-constructed (all-null) Frame_Data.
        void Destroy(VkDevice _device, VmaAllocator _allocator)
        {
            assert(_device != VK_NULL_HANDLE &&
                "Frame_Data::Destroy() called with a null device");

            Vulkan_Buffer_Utils::Destroy_buffer(_allocator, light_buffer);
            Vulkan_Buffer_Utils::Destroy_buffer(_allocator, uniform_buffer);

            if (present_fence != VK_NULL_HANDLE) {
                vkDestroyFence(_device, present_fence, nullptr);
                present_fence = VK_NULL_HANDLE;
            }
            if (in_flight_fence != VK_NULL_HANDLE) {
                vkDestroyFence(_device, in_flight_fence, nullptr);
                in_flight_fence = VK_NULL_HANDLE;
            }
            if (render_finished_semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(_device, render_finished_semaphore, nullptr);
                render_finished_semaphore = VK_NULL_HANDLE;
            }
            if (image_available_semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(_device, image_available_semaphore, nullptr);
                image_available_semaphore = VK_NULL_HANDLE;
            }
            if (command_pool != VK_NULL_HANDLE) {
                // Destroying the pool frees all command buffers from it.
                vkDestroyCommandPool(_device, command_pool, nullptr);
                command_pool = VK_NULL_HANDLE;
                command_buffer = VK_NULL_HANDLE;
            }
        }
    };

} // namespace Renderer