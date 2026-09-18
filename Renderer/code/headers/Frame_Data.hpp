#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Utils.hpp>
#include <Matrix.hpp>

#include <cassert>
#include <cstdint>
#include <stdexcept>

namespace Renderer_System
{

    // Frame_UBO: per-frame uniform data uploaded to the GPU each frame.
    // Contains view and projection matrices used by the vertex shader.
    // The model matrix is handled separately (push constants or per-object
    // buffer) — not here.
    struct Frame_UBO
    {
        MathLib::Matrix4 view;
        MathLib::Matrix4 projection;
    };

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
    // (valid for HOST_COHERENT memory). The Renderer writes into
    // uniform_mapped_ptr directly each frame with std::memcpy — no
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

        VkBuffer       uniform_buffer = VK_NULL_HANDLE;
        VkDeviceMemory uniform_memory = VK_NULL_HANDLE;

        // Persistent CPU-side pointer into uniform_memory.
        // Valid for the entire lifetime of this Frame_Data.
        // Write here each frame: std::memcpy(uniform_mapped_ptr, &ubo, sizeof(ubo))
        void* uniform_mapped_ptr = nullptr;

        // =========================================================
        // Lifecycle
        // =========================================================

        // Allocates all resources for this frame slot.
        void Init(const Vulkan_Device& _device)
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
            Vulkan_Buffer_Utils::Create_buffer(
                _device,
                sizeof(Frame_UBO),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                uniform_buffer,
                uniform_memory
            );

            // Map once, keep mapped for the lifetime of this frame slot.
            vkMapMemory(device, uniform_memory, 0, sizeof(Frame_UBO), 0, &uniform_mapped_ptr);
        }

        // Destroys all resources owned by this frame slot.
        // Safe to call on a default-constructed (all-null) Frame_Data.
        void Destroy(VkDevice _device)
        {
            assert(_device != VK_NULL_HANDLE &&
                "Frame_Data::Destroy() called with a null device");

            if (uniform_mapped_ptr != nullptr) {
                vkUnmapMemory(_device, uniform_memory);
                uniform_mapped_ptr = nullptr;
            }
            if (uniform_buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(_device, uniform_buffer, nullptr);
                uniform_buffer = VK_NULL_HANDLE;
            }
            if (uniform_memory != VK_NULL_HANDLE) {
                vkFreeMemory(_device, uniform_memory, nullptr);
                uniform_memory = VK_NULL_HANDLE;
            }
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