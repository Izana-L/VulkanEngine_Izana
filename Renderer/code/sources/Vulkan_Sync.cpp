#include "Vulkan_Sync.hpp"
#include "Vulkan_Utils.hpp"

#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer {

    // ---------- Constructor ----------
    Vulkan_Sync::Vulkan_Sync(const Vulkan_Device& _device,uint32_t _max_frames_in_flight,
                                uint32_t _swapchain_image_count,bool _use_present_fences)
                                : device_handle(_device.Get_logical_device_handle()) 
    {

        assert(device_handle != VK_NULL_HANDLE && "...");
        assert(_max_frames_in_flight >= 1 && "...");
        assert(_swapchain_image_count >= 1 && "...");

        image_available_semaphores.resize(_swapchain_image_count);
        render_finished_semaphores.resize(_max_frames_in_flight);
        in_flight_fences.resize(_max_frames_in_flight);

        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < _swapchain_image_count; ++i) {
            VkResult result = vkCreateSemaphore(device_handle, &semaphore_info, nullptr, &image_available_semaphores[i]);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create image_available semaphore " + std::to_string(i) + ": " +
                    Vulkan_utils::Vk_result_to_string(result)
                );
            }
        }

        for (uint32_t i = 0; i < _max_frames_in_flight; ++i) {
            VkResult result = vkCreateSemaphore(device_handle, &semaphore_info, nullptr, &render_finished_semaphores[i]);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create render_finished semaphore " + std::to_string(i) + ": " +
                    Vulkan_utils::Vk_result_to_string(result)
                );
            }

            result = vkCreateFence(device_handle, &fence_info, nullptr, &in_flight_fences[i]);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create in_flight fence " + std::to_string(i) + ": " +
                    Vulkan_utils::Vk_result_to_string(result)
                );
            }
        }

        // Present fences: only created if VK_KHR_swapchain_maintenance1
        // is available. Start signaled so the first Wait_and_reset call
        // returns immediately (same reasoning as in_flight_fences).
        if (_use_present_fences) {
            present_fences.resize(_swapchain_image_count);
            for (uint32_t i = 0; i < _swapchain_image_count; ++i) {
                VkResult result = vkCreateFence(device_handle, &fence_info, nullptr, &present_fences[i]);
                if (result != VK_SUCCESS) {
                    throw std::runtime_error(
                        "Failed to create present fence " + std::to_string(i) + ": " +
                        Vulkan_utils::Vk_result_to_string(result)
                    );
                }
            }
            std::cout << "[Vulkan_Sync] Present fences created (" << _swapchain_image_count << ").\n";
        }

        std::cout << "[Vulkan_Sync] Created sync objects: "
            << _swapchain_image_count << " image_available semaphore(s), "
            << _max_frames_in_flight << " render_finished semaphore(s) and fence(s).\n";
    }

    // ---------- Destructor ----------
    Vulkan_Sync::~Vulkan_Sync() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Sync::Destroy() {
        for (VkSemaphore semaphore : image_available_semaphores) {
            vkDestroySemaphore(device_handle, semaphore, nullptr);
        }
        for (VkSemaphore semaphore : render_finished_semaphores) {
            vkDestroySemaphore(device_handle, semaphore, nullptr);
        }
        for (VkFence fence : in_flight_fences) {
            vkDestroyFence(device_handle, fence, nullptr);
        }
        for (VkFence fence : present_fences) {
            vkDestroyFence(device_handle, fence, nullptr);
        }
       
        image_available_semaphores.clear();
        render_finished_semaphores.clear();
        in_flight_fences.clear();
        present_fences.clear();
    }

    // ---------- Move constructor ----------
    Vulkan_Sync::Vulkan_Sync(Vulkan_Sync&& _other) noexcept
        : device_handle(_other.device_handle),
        image_available_semaphores(std::move(_other.image_available_semaphores)),
        render_finished_semaphores(std::move(_other.render_finished_semaphores)),
        in_flight_fences(std::move(_other.in_flight_fences)), 
        present_fences(std::move(_other.present_fences))
    {

        _other.image_available_semaphores.clear();
        _other.render_finished_semaphores.clear();
        _other.in_flight_fences.clear();
        _other.present_fences.clear();
    }

    // ---------- Move assignment ----------
    Vulkan_Sync& Vulkan_Sync::operator=(Vulkan_Sync&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            image_available_semaphores = std::move(_other.image_available_semaphores);
            render_finished_semaphores = std::move(_other.render_finished_semaphores);
            in_flight_fences = std::move(_other.in_flight_fences);

            _other.image_available_semaphores.clear();
            _other.render_finished_semaphores.clear();
            _other.in_flight_fences.clear();
        }
        return *this;
    }

    // ---------- Get_image_available_semaphore ----------
    // NOTE: indexed by IMAGE_INDEX, not frame_index
    VkSemaphore Vulkan_Sync::Get_image_available_semaphore(uint32_t _image_index) const {
        assert(!image_available_semaphores.empty() && "Get_image_available_semaphore() called on a moved-from or destroyed Vulkan_Sync");
        assert(_image_index < image_available_semaphores.size() && "Get_image_available_semaphore() called with an out-of-range image index");
        return image_available_semaphores[_image_index];
    }

    // ---------- Get_render_finished_semaphore ----------
    // NOTE: indexed by FRAME_INDEX
    VkSemaphore Vulkan_Sync::Get_render_finished_semaphore(uint32_t _frame_index) const {
        assert(!render_finished_semaphores.empty() && "Get_render_finished_semaphore() called on a moved-from or destroyed Vulkan_Sync");
        assert(_frame_index < render_finished_semaphores.size() && "Get_render_finished_semaphore() called with an out-of-range frame index");
        return render_finished_semaphores[_frame_index];
    }

    // ---------- Get_in_flight_fence ----------
    // NOTE: indexed by FRAME_INDEX
    VkFence Vulkan_Sync::Get_in_flight_fence(uint32_t _frame_index) const {
        assert(!in_flight_fences.empty() && "Get_in_flight_fence() called on a moved-from or destroyed Vulkan_Sync");
        assert(_frame_index < in_flight_fences.size() && "Get_in_flight_fence() called with an out-of-range frame index");
        return in_flight_fences[_frame_index];
    }

    // ---------- Wait_for_fence ----------
    void Vulkan_Sync::Wait_for_fence(uint32_t _frame_index) const {
        assert(!in_flight_fences.empty() && "Wait_for_fence() called on a moved-from or destroyed Vulkan_Sync");
        assert(_frame_index < in_flight_fences.size() && "Wait_for_fence() called with an out-of-range frame index");
        vkWaitForFences(device_handle, 1, &in_flight_fences[_frame_index], VK_TRUE, UINT64_MAX);
    }

    // ---------- Reset_fence ----------
    void Vulkan_Sync::Reset_fence(uint32_t _frame_index) const {
        assert(!in_flight_fences.empty() && "Reset_fence() called on a moved-from or destroyed Vulkan_Sync");
        assert(_frame_index < in_flight_fences.size() && "Reset_fence() called with an out-of-range frame index");
        vkResetFences(device_handle, 1, &in_flight_fences[_frame_index]);
    }

    // ---------- Get_max_frames_in_flight ----------
    uint32_t Vulkan_Sync::Get_max_frames_in_flight() const {
        assert(!in_flight_fences.empty() && "Get_max_frames_in_flight() called on a moved-from or destroyed Vulkan_Sync");
        return static_cast<uint32_t>(in_flight_fences.size());
    }

    // ---------- Get_swapchain_image_count ----------
    uint32_t Vulkan_Sync::Get_swapchain_image_count() const {
        assert(!image_available_semaphores.empty() && "Get_swapchain_image_count() called on a moved-from or destroyed Vulkan_Sync");
        return static_cast<uint32_t>(image_available_semaphores.size());
    }
    VkFence Vulkan_Sync::Get_present_fence(uint32_t _image_index) const {
        assert(!present_fences.empty() && "Get_present_fence() called but present fences were not created");
        assert(_image_index < present_fences.size() && "Get_present_fence() called with out-of-range image index");
        return present_fences[_image_index];
    }

    bool Vulkan_Sync::Has_present_fences() const {
        return !present_fences.empty();
    }

    void Vulkan_Sync::Wait_and_reset_present_fence(uint32_t _image_index) const {
        assert(!present_fences.empty() && "Wait_and_reset_present_fence() called but present fences were not created");
        assert(_image_index < present_fences.size() && "Wait_and_reset_present_fence() called with out-of-range index");

        vkWaitForFences(device_handle, 1, &present_fences[_image_index], VK_TRUE, UINT64_MAX);
        vkResetFences(device_handle, 1, &present_fences[_image_index]);
    }
}