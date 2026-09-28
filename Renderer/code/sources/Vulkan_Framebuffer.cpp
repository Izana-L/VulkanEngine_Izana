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
        const Vulkan_Depth_Resources& _depth_resources,
        const Vulkan_OIT_Resources& _oit_resources)

        : device_handle(_device.Get_logical_device_handle()) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating framebuffers");

        Create(_render_pass, _swapchain, _depth_resources, _oit_resources);
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
        const Vulkan_Depth_Resources& _depth_resources,
        const Vulkan_OIT_Resources& _oit_resources) {

        const std::vector<VkImageView>& color_image_views = _swapchain.Get_image_views();
        VkExtent2D extent = _swapchain.Get_extent();

        framebuffers.resize(color_image_views.size());

        // Create one framebuffer per swapchain color image view. Each
        // framebuffer shares the SAME depth image view and the SAME two OIT
        // targets.
        //
        // This is safe, but NOT because only one frame touches them at a
        // time: with FRAMES_IN_FLIGHT = 2, two submissions can be executing
        // on the GPU simultaneously. What makes it safe are the external
        // subpass dependencies of Vulkan_Render_Pass. Their srcSubpass is
        // VK_SUBPASS_EXTERNAL, which covers everything submitted earlier on
        // the queue, the previous frame included:
        //   - EXTERNAL -> opaque: srcStageMask includes the fragment test
        //     stages and COLOR_ATTACHMENT_OUTPUT, so the previous frame's
        //     depth writes and tests are ordered before this frame's depth
        //     clear;
        //   - EXTERNAL -> transparent: srcStageMask includes FRAGMENT_SHADER
        //     and COLOR_ATTACHMENT_OUTPUT, so the previous frame's composite
        //     reads and accumulation writes are ordered before this frame's
        //     clear of the OIT targets.
        //
        // Consequence: the two frames in flight overlap CPU/GPU work, and
        // the GPU work of one frame outside the render pass (compute,
        // transfers) with the render pass of the other, but not two render
        // passes. If either dependency is ever relaxed, or another render
        // pass uses these images, they must be revisited.
        for (size_t i = 0; i < color_image_views.size(); ++i) {
            // Render_Pass_Attachment order.
            std::array<VkImageView, Render_Pass_Attachment::Count> attachments{};
            attachments[Render_Pass_Attachment::Color] = color_image_views[i];
            attachments[Render_Pass_Attachment::Depth] = _depth_resources.Get_image_view();
            attachments[Render_Pass_Attachment::Oit_Accumulation] = _oit_resources.Get_accumulation_view();
            attachments[Render_Pass_Attachment::Oit_Revealage] = _oit_resources.Get_revealage_view();

            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;

            // The framebuffer must be created against a SPECIFIC render
            // pass, and its attachments must match that render pass's
            // attachment count/order exactly (Render_Pass_Attachment).
            framebuffer_info.renderPass = _render_pass.Get_handle();
            framebuffer_info.attachmentCount = static_cast<uint32_t>(attachments.size());
            framebuffer_info.pAttachments = attachments.data();
            framebuffer_info.width = extent.width;
            framebuffer_info.height = extent.height;
            framebuffer_info.layers = 1; // not using multiview/stereo rendering

            VK_CHECK(vkCreateFramebuffer(device_handle, &framebuffer_info, nullptr, &framebuffers[i]),
                ("Failed to create framebuffer " + std::to_string(i)).c_str());
        }

        std::cout << "[Vulkan_Framebuffer] Created " << framebuffers.size() << " framebuffer(s), size "
            << extent.width << "x" << extent.height << "\n";
    }

    // ---------- Recreate ----------
    void Vulkan_Framebuffer::Recreate(
        const Vulkan_Render_Pass& _render_pass,
        const Vulkan_Swapchain& _swapchain,
        const Vulkan_Depth_Resources& _depth_resources,
        const Vulkan_OIT_Resources& _oit_resources) {

        assert(!framebuffers.empty() && "Recreate() called on a moved-from or already-destroyed Vulkan_Framebuffer");

        Destroy();
        Create(_render_pass, _swapchain, _depth_resources, _oit_resources);

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