#include <Present_Sync.hpp>
#include <Vulkan_Utils.hpp>

#include <cassert>
#include <cstddef>
#include <iostream>
#include <utility>

namespace Renderer_System
{

    namespace
    {
        // Frames a retired semaphore has to survive before it is destroyed
        // when no present fence can prove its present completed: every
        // frame slot plus every swapchain image has cycled by then.
        constexpr uint32_t RETIRED_SYNC_GRACE_FRAMES = 8;

        // How long a shutdown or a recreation waits for a present fence
        // before giving up on it (nanoseconds). A driver never signaling a
        // present fence is a driver fault; the wait must not hang forever.
        constexpr uint64_t PRESENT_FENCE_TIMEOUT_NS = 1'000'000'000ull;
    }

    Present_Sync::Present_Sync(const Vulkan_Device& _device)
        : device(_device)
    {
    }

    Present_Sync::~Present_Sync()
    {
        // Wait for pending presents where a fence can prove completion,
        // then destroy everything.
        Retire();
        Flush(true);
    }

    void Present_Sync::Create(uint32_t _image_count)
    {
        assert(image_sync.empty() && "Present_Sync::Create: the previous set must be retired first");

        VkDevice dev = device.Get_logical_device_handle();
        const bool use_present_fences = device.Is_swapchain_maintenance1_enabled();

        image_sync.assign(_image_count, Image_Sync{});

        for (Image_Sync& sync : image_sync)
        {
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            VK_CHECK(vkCreateSemaphore(dev, &semaphore_info, nullptr, &sync.render_finished),
                "Renderer: failed to create render_finished semaphore");

            if (use_present_fences)
            {
                // Not pre-signaled: only a real present signals it, and
                // present_pending records whether one is outstanding.
                VkFenceCreateInfo fence_info{};
                fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

                VK_CHECK(vkCreateFence(dev, &fence_info, nullptr, &sync.present_fence),
                    "Renderer: failed to create present fence");
            }
        }
    }

    Present_Sync::Image_Sync& Present_Sync::Get(uint32_t _image_index)
    {
        assert(_image_index < image_sync.size() && "Present_Sync::Get: image index out of range");

        return image_sync[_image_index];
    }

    void Present_Sync::Wait_for_previous_present(uint32_t _image_index)
    {
        Image_Sync& sync = Get(_image_index);

        // With swapchain_maintenance1 the present operation signals
        // present_fence when done. Waiting on it before reusing
        // render_finished (which that present waits on) is what makes
        // re-signaling the semaphore legal. present_pending is only ever
        // true when the fence exists.
        if (!sync.present_pending)
            return;

        VkDevice dev = device.Get_logical_device_handle();

        VK_CHECK(vkWaitForFences(dev, 1, &sync.present_fence, VK_TRUE, UINT64_MAX),
            "Render: wait for present fence");
        VK_CHECK(vkResetFences(dev, 1, &sync.present_fence), "Render: reset present fence");
        sync.present_pending = false;
    }

    void Present_Sync::Retire()
    {
        VkDevice dev = device.Get_logical_device_handle();

        Retired_Sync retired;
        retired.frames_left = RETIRED_SYNC_GRACE_FRAMES;

        for (Image_Sync& sync : image_sync)
        {
            if (sync.present_fence != VK_NULL_HANDLE)
            {
                // With a present fence the completion of the last present
                // of this image is known exactly: wait for it, then both
                // objects can be destroyed immediately.
                bool completed = true;

                if (sync.present_pending)
                {
                    const VkResult wait = vkWaitForFences(dev, 1, &sync.present_fence, VK_TRUE, PRESENT_FENCE_TIMEOUT_NS);

                    if (wait == VK_SUCCESS)
                        sync.present_pending = false;
                    else
                    {
                        // Timeout or device loss: the fence may still be
                        // pending, so it must not be destroyed yet.
                        completed = false;
                        std::cerr << "[Renderer] Present fence not signaled before swapchain retirement: "
                            << Vulkan_Utils::Vk_result_to_string(wait) << "\n";
                    }
                }

                if (completed)
                {
                    vkDestroyFence(dev, sync.present_fence, nullptr);
                    vkDestroySemaphore(dev, sync.render_finished, nullptr);
                }
                else
                {
                    retired.fences.push_back(sync.present_fence);
                    retired.semaphores.push_back(sync.render_finished);
                }
            }
            else
            {
                // No present fence: nothing says when the presentation
                // engine is done waiting on this semaphore, so it is kept
                // alive for a grace period instead of destroyed now.
                retired.semaphores.push_back(sync.render_finished);
            }

            sync = Image_Sync{};
        }

        image_sync.clear();

        if (!retired.semaphores.empty() || !retired.fences.empty())
            retired_sync.push_back(std::move(retired));
    }

    void Present_Sync::Flush(bool _force)
    {
        VkDevice dev = device.Get_logical_device_handle();

        for (size_t i = 0; i < retired_sync.size(); )
        {
            Retired_Sync& retired = retired_sync[i];

            if (retired.frames_left > 0) --retired.frames_left;

            bool fences_done = true;

            for (VkFence fence : retired.fences)
            {
                if (_force)
                {
                    // Last chance: wait, bounded, then destroy regardless.
                    // A fence that never signals cannot be waited on forever
                    // at shutdown.
                    const VkResult wait = vkWaitForFences(dev, 1, &fence, VK_TRUE, PRESENT_FENCE_TIMEOUT_NS);
                    if (wait != VK_SUCCESS)
                        std::cerr << "[Renderer] Retired present fence still pending at shutdown: "
                            << Vulkan_Utils::Vk_result_to_string(wait) << "\n";
                }
                else if (vkGetFenceStatus(dev, fence) != VK_SUCCESS)
                {
                    fences_done = false;
                }
            }

            const bool ready = _force || (retired.frames_left == 0 && fences_done);

            if (!ready)
            {
                ++i;
                continue;
            }

            for (VkFence fence : retired.fences)
                vkDestroyFence(dev, fence, nullptr);

            for (VkSemaphore semaphore : retired.semaphores)
                vkDestroySemaphore(dev, semaphore, nullptr);

            retired_sync.erase(retired_sync.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

} // namespace Renderer_System
