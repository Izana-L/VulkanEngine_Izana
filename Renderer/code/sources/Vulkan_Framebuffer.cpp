#include <Vulkan_Framebuffer.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <array>
#include <cassert>
#include <string>

namespace Renderer_System {

    namespace
    {
        // Throws unless _actual is the format the render pass declares for
        // the attachment named _attachment.
        void Require_attachment_format(VkFormat _actual, VkFormat _expected, const char* _attachment)
        {
            if (_actual == _expected)
                return;

            throw std::runtime_error(std::string("Vulkan_Framebuffer: the ") + _attachment + " attachment has format " +
                                     Vulkan_Utils::Vk_format_to_string(_actual) + ", but the render pass was created for " +
                                     Vulkan_Utils::Vk_format_to_string(_expected));
        }
    }

    // ---------- Constructor ----------
    Vulkan_Framebuffer::Vulkan_Framebuffer(
        const Vulkan_Device& _device,
        const Vulkan_Render_Pass& _render_pass,
        const Vulkan_Swapchain& _swapchain,
        const Vulkan_Depth_Resources& _depth_resources,
        const Vulkan_OIT_Resources& _oit_resources)

        : framebuffers(Create(_device.Get_logical_device_handle(), _render_pass, _swapchain, _depth_resources, _oit_resources)) {

        assert(_device.Get_logical_device_handle() != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating framebuffers");
    }

    // ---------- Create ----------
    std::vector<Unique_Framebuffer> Vulkan_Framebuffer::Create(
        VkDevice _device,
        const Vulkan_Render_Pass& _render_pass,
        const Vulkan_Swapchain& _swapchain,
        const Vulkan_Depth_Resources& _depth_resources,
        const Vulkan_OIT_Resources& _oit_resources) {

        // Before anything is created: the views must be of the formats the
        // render pass was built with.
        Require_attachment_format(_swapchain.Get_image_format(), _render_pass.Get_color_format(), "color (swapchain)");
        Require_attachment_format(_depth_resources.Get_format(), _render_pass.Get_depth_format(), "depth");

        const uint32_t image_count = _swapchain.Get_image_count();
        VkExtent2D extent = _swapchain.Get_extent();

        // Built in a local vector: if the creation of framebuffer i throws,
        // the vector destroys the i - 1 wrappers that already own theirs.
        // The handle of a failed vkCreateFramebuffer is never stored: a
        // wrapper is constructed only from a handle that was created
        // successfully. The capacity is reserved up front, so appending a
        // wrapper cannot throw between the creation and the ownership.
        std::vector<Unique_Framebuffer> created;
        created.reserve(image_count);

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
        for (uint32_t i = 0; i < image_count; ++i) {
            // Render_Pass_Attachment order.
            std::array<VkImageView, Render_Pass_Attachment::Count> attachments{};
            attachments[Render_Pass_Attachment::Color] = _swapchain.Get_image_view(i);
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

            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VK_CHECK(vkCreateFramebuffer(_device, &framebuffer_info, nullptr, &framebuffer),
                ("Failed to create framebuffer " + std::to_string(i)).c_str());

            created.emplace_back(_device, framebuffer);
        }

        std::cout << "[Vulkan_Framebuffer] Created " << created.size() << " framebuffer(s), size "
            << extent.width << "x" << extent.height << "\n";

        return created;
    }

    // ---------- Get_framebuffer ----------
    VkFramebuffer Vulkan_Framebuffer::Get_framebuffer(uint32_t _image_index) const {
        assert(!framebuffers.empty() && "Get_framebuffer() called on a moved-from or destroyed Vulkan_Framebuffer");
        assert(_image_index < framebuffers.size() && "Get_framebuffer() called with an out-of-range image index");

        return framebuffers[_image_index].Get();
    }

    // ---------- Get_count ----------
    uint32_t Vulkan_Framebuffer::Get_count() const {
        assert(!framebuffers.empty() && "Get_count() called on a moved-from or destroyed Vulkan_Framebuffer");
        return static_cast<uint32_t>(framebuffers.size());
    }

}
