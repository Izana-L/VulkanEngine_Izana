#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Vulkan_Device.hpp"

#include <vector>

namespace Renderer_System {

    // Vulkan_Sync: owns the synchronization primitives needed to
    // coordinate CPU and GPU work across frames-in-flight.
    //
    // - image_available semaphores: ONE PER SWAPCHAIN IMAGE (not per
    //   frame-in-flight). Indexed by image_index returned from
    //   Acquire_next_image(). This avoids reusing a semaphore that the
    //   swapchain's presentation engine may still be consuming.
    //
    // - render_finished semaphores: one per frame-in-flight. Signaled
    //   when the GPU finishes rendering, consumed by Present().
    //
    // - in_flight fences: one per frame-in-flight. Signaled when the
    //   GPU finishes the entire frame submission, used by the CPU to
    //   know when it's safe to reuse that frame's resources.
    class Vulkan_Sync 
    {
        VkDevice device_handle;

        // Indexed by image_index (swapchain image count)
        std::vector<VkSemaphore> image_available_semaphores;

        // Indexed by frame_index (max_frames_in_flight)
        std::vector<VkSemaphore> render_finished_semaphores;
        std::vector<VkFence> in_flight_fences;
        // Indexed by image_index - only populated if maintenance1 is enabled
        std::vector<VkFence> present_fences;

    public:
        // Creates:
        // - _swapchain_image_count image_available semaphores
        // - _max_frames_in_flight render_finished semaphores
        // - _max_frames_in_flight in_flight fences (pre-signaled)
        Vulkan_Sync(
            const Vulkan_Device& _device,
            uint32_t _max_frames_in_flight,
            uint32_t _swapchain_image_count,
            bool _use_present_fences);

        ~Vulkan_Sync();

        Vulkan_Sync(const Vulkan_Sync&) = delete;
        Vulkan_Sync& operator=(const Vulkan_Sync&) = delete;

        Vulkan_Sync(Vulkan_Sync&& _other) noexcept;
        Vulkan_Sync& operator=(Vulkan_Sync&& _other) noexcept;

        // Semaphore signaled by the GPU once a specific swapchain image
        // is ready to be rendered into. Indexed by IMAGE_INDEX (the
        // value returned from Acquire_next_image()), NOT frame_index.
        // This prevents reusing a semaphore still in use by a previous
        // presentation of the same swapchain image.
        VkSemaphore Get_image_available_semaphore(uint32_t _image_index) const;

        // Semaphore signaled by the GPU once rendering has finished.
        // Indexed by FRAME_INDEX (current_frame % MAX_FRAMES_IN_FLIGHT).
        VkSemaphore Get_render_finished_semaphore(uint32_t _frame_index) const;

        // Fence signaled by the GPU once the entire frame submission
        // for this slot is complete. Indexed by FRAME_INDEX.
        VkFence Get_in_flight_fence(uint32_t _frame_index) const;

        // Blocks until the given frame's fence is signaled.
        void Wait_for_fence(uint32_t _frame_index) const;

        // Resets the given frame's fence to unsignaled.
        void Reset_fence(uint32_t _frame_index) const;

        // How many frames-in-flight this object manages
        uint32_t Get_max_frames_in_flight() const;

        // How many image_available semaphores were created
        // (matches swapchain image count)
        uint32_t Get_swapchain_image_count() const;

        // Presentation fences: one per swapchain image. Used with
        // VK_KHR_swapchain_maintenance1 to know exactly when the
        // presentation engine has finished with each image's semaphore,
        // allowing safe reuse of image_available semaphores with MAILBOX.
        // Only created if VK_KHR_swapchain_maintenance1 is enabled.
        VkFence Get_present_fence(uint32_t _image_index) const;

        // Returns true if presentation fences were created
        bool Has_present_fences() const;

        // Waits for the present fence of the given image index, then
        // resets it - call this before reusing that image's semaphore.
        void Wait_and_reset_present_fence(uint32_t _image_index) const;

    private:
        void Destroy();

        
    };

}