#include <Vulkan_Framebuffer.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <array>
#include <cassert>

namespace Renderer_System {

    // ---------- Constructor ----------
    Vulkan_Framebuffer::Vulkan_Framebuffer(
        const Vulkan_Device& _device,
        const Vulkan_Render_Pass& _render_pass,
        const Vulkan_Swapchain& _swapchain,
        const Vulkan_Depth_Resources& _depth_resources)

        : device_handle(_device.Get_logical_device_handle()) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating framebuffers");

        Create(_render_pass, _swapchain, _depth_resources);
    }

    // ---------- Destructor ----------
    Vulkan_Framebuffer::~Vulkan_Framebuffer() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Framebuffer::Destroy() {
        for (VkFramebuffer framebuffer : framebuffers) {
            vkDestroyFramebuffer(device_handle, framebuffer, nullptr);
        }
        framebuffers.clear();
    }

    // ---------- Create ----------
    void Vulkan_Framebuffer::Create(
        const Vulkan_Render_Pass& _render_pass,
        const Vulkan_Swapchain& _swapchain,
        const Vulkan_Depth_Resources& _depth_resources) {

        const std::vector<VkImageView>& color_image_views = _swapchain.Get_image_views();
        VkExtent2D extent = _swapchain.Get_extent();

        framebuffers.resize(color_image_views.size());

        // Create one framebuffer per swapchain color image view. Each
        // framebuffer shares the SAME depth image view - this is valid
        // and expected, since only one frame is ever actively being
        // rendered to the depth buffer at a time (we don't need a
        // separate depth buffer per swapchain image).
        for (size_t i = 0; i < color_image_views.size(); ++i) {
            std::array<VkImageView, 2> attachments = {
                color_image_views[i],
                _depth_resources.Get_image_view()
            };

            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;

            // The framebuffer must be created against a SPECIFIC render
            // pass, and its attachments must match that render pass's
            // attachment count/order exactly (color at index 0, depth at
            // index 1, matching how Vulkan_Render_Pass defined them).
            framebuffer_info.renderPass = _render_pass.Get_handle();
            framebuffer_info.attachmentCount = static_cast<uint32_t>(attachments.size());
            framebuffer_info.pAttachments = attachments.data();
            framebuffer_info.width = extent.width;
            framebuffer_info.height = extent.height;
            framebuffer_info.layers = 1; // not using multiview/stereo rendering

            VkResult result = vkCreateFramebuffer(device_handle, &framebuffer_info, nullptr, &framebuffers[i]);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create framebuffer " + std::to_string(i) + ": " +
                    Vulkan_Utils::Vk_result_to_string(result)
                );
            }
        }

        std::cout << "[Vulkan_Framebuffer] Created " << framebuffers.size() << " framebuffer(s), size "
            << extent.width << "x" << extent.height << "\n";
    }

    // ---------- Recreate ----------
    void Vulkan_Framebuffer::Recreate(
        const Vulkan_Render_Pass& _render_pass,
        const Vulkan_Swapchain& _swapchain,
        const Vulkan_Depth_Resources& _depth_resources) {

        assert(!framebuffers.empty() && "Recreate() called on a moved-from or already-destroyed Vulkan_Framebuffer");

        Destroy();
        Create(_render_pass, _swapchain, _depth_resources);

        std::cout << "[Vulkan_Framebuffer] Framebuffers recreated.\n";
    }

    // ---------- Move constructor ----------
    Vulkan_Framebuffer::Vulkan_Framebuffer(Vulkan_Framebuffer&& _other) noexcept
        : device_handle(_other.device_handle),
        framebuffers(std::move(_other.framebuffers)) {

        _other.framebuffers.clear();
    }

    // ---------- Move assignment ----------
    Vulkan_Framebuffer& Vulkan_Framebuffer::operator=(Vulkan_Framebuffer&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            framebuffers = std::move(_other.framebuffers);

            _other.framebuffers.clear();
        }
        return *this;
    }

    // ---------- Get_framebuffer ----------
    VkFramebuffer Vulkan_Framebuffer::Get_framebuffer(uint32_t _image_index) const {
        assert(!framebuffers.empty() && "Get_framebuffer() called on a moved-from or destroyed Vulkan_Framebuffer");
        assert(_image_index < framebuffers.size() && "Get_framebuffer() called with an out-of-range image index");

        return framebuffers[_image_index];
    }

    // ---------- Get_count ----------
    uint32_t Vulkan_Framebuffer::Get_count() const {
        assert(!framebuffers.empty() && "Get_count() called on a moved-from or destroyed Vulkan_Framebuffer");
        return static_cast<uint32_t>(framebuffers.size());
    }

}