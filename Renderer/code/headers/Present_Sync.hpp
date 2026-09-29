#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Device.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Present_Sync: the synchronization objects of presentation, owned per
    // SWAPCHAIN IMAGE and not per frame slot.
    //
    // A present may still be waiting on the "render finished" semaphore of
    // an image while the frame slot that recorded it is reused, and there
    // are more images than slots, so per-slot semaphores would be
    // re-signaled while pending. With VK_KHR_swapchain_maintenance1 the
    // present fence tells exactly when the present of an image completed;
    // without it, per-image semaphores are the guarantee, and the
    // semaphores of a destroyed swapchain are kept for a few frames before
    // being destroyed.
    //
    // Lifetime: the objects are tied to the current swapchain. Create()
    // builds one set per image after the swapchain exists; Retire() moves
    // them out of service before the swapchain is rebuilt; Flush() destroys
    // retired objects once it is safe. The destructor retires whatever is
    // live and destroys everything, so it must run before the swapchain
    // is destroyed and after the device went idle.
    //
    // Not copyable or movable.
    class Present_Sync
    {
    public:

        // Per swapchain image.
        struct Image_Sync
        {
            // Signaled by the graphics queue when rendering into this image
            // completes; the present of this image waits on it.
            VkSemaphore render_finished = VK_NULL_HANDLE;

            // VK_KHR_swapchain_maintenance1 only: signaled when the present
            // of this image completes. VK_NULL_HANDLE otherwise.
            VkFence     present_fence = VK_NULL_HANDLE;

            // True while a present that signals present_fence is pending.
            // Set by the Renderer only when vkQueuePresentKHR queued the
            // present (its result is SUCCESS, SUBOPTIMAL, OUT_OF_DATE or
            // SURFACE_LOST): a present that failed without queueing anything
            // never signals the fence, and waiting for it would hang.
            bool        present_pending = false;
        };

        explicit Present_Sync(const Vulkan_Device& _device);
        ~Present_Sync();

        Present_Sync(const Present_Sync&) = delete;
        Present_Sync& operator=(const Present_Sync&) = delete;
        Present_Sync(Present_Sync&&) = delete;
        Present_Sync& operator=(Present_Sync&&) = delete;

        // Creates one Image_Sync per image of the CURRENT swapchain,
        // replacing the live set, which must have been retired first.
        void Create(uint32_t _image_count);

        // Moves the live Image_Sync objects out of service: the ones whose
        // present is known to have completed are destroyed at once, the
        // rest go to the retired list.
        void Retire();

        // Destroys retired objects whose grace period has elapsed; called
        // once per frame. _force (shutdown) waits, bounded, for any pending
        // present fence and destroys everything.
        void Flush(bool _force);

        // The objects of swapchain image _image_index.
        Image_Sync& Get(uint32_t _image_index);

        // Blocks until the previous present of the image completed, then
        // rearms its fence. Precondition of re-signaling render_finished,
        // which that present waits on. Does nothing when no present is
        // pending, which is always the case without
        // VK_KHR_swapchain_maintenance1: no fence exists to wait on.
        void Wait_for_previous_present(uint32_t _image_index);

    private:

        // Objects of a swapchain that no longer exists but whose last
        // presents may still be pending. Destroyed after frames_left
        // further Flush calls (or when their fences report the present
        // completed).
        struct Retired_Sync
        {
            std::vector<VkSemaphore> semaphores;
            std::vector<VkFence>     fences;
            uint32_t                 frames_left = 0;
        };

        const Vulkan_Device&       device;

        std::vector<Image_Sync>    image_sync;
        std::vector<Retired_Sync>  retired_sync;
    };

} // namespace Renderer_System
