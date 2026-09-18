#include <Vulkan_Render_Pass.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <array>
#include <cassert>

namespace Renderer_System {

    // ---------- Constructor ----------
    Vulkan_Render_Pass::Vulkan_Render_Pass(
        const Vulkan_Device& _device,
        VkFormat _color_format,
        VkFormat _depth_format)

        : device_handle(_device.Get_logical_device_handle()),
        render_pass(VK_NULL_HANDLE),
        depth_format(_depth_format) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a render pass");
        assert(_color_format != VK_FORMAT_UNDEFINED && "Color format cannot be VK_FORMAT_UNDEFINED");
        assert(_depth_format != VK_FORMAT_UNDEFINED && "Depth format cannot be VK_FORMAT_UNDEFINED");

        // ---------- Color attachment ----------
        // Describes the swapchain image we'll be drawing color into.
        VkAttachmentDescription color_attachment{};
        color_attachment.format = _color_format;
        color_attachment.samples = VK_SAMPLE_COUNT_1_BIT; // no MSAA for now

        // loadOp: what to do with the attachment's previous contents when
        // the render pass begins. CLEAR wipes it to a solid color (the
        // clear color we'll specify when recording command buffers later).
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;

        // storeOp: what to do with the contents after the render pass
        // ends. STORE keeps them - we need this since we're about to
        // present this image to the screen.
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        // Stencil load/store ops don't apply to a color attachment -
        // DONT_CARE means "we don't use this, Vulkan can do whatever is cheapest".
        color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

        // initialLayout: what layout the image is expected to be in when
        // the render pass starts. UNDEFINED means "we don't care about/
        // don't need to preserve whatever was there before" - appropriate
        // since we're clearing it anyway.
        color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // finalLayout: what layout the image must be transitioned to by
        // the time the render pass ends. PRESENT_SRC_KHR is exactly the
        // layout the swapchain needs for vkQueuePresentKHR to work.
        color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        // ---------- Depth attachment ----------
        // Describes the depth buffer used for depth testing (so closer
        // objects correctly occlude farther ones when multiple objects
        // overlap on screen).
        VkAttachmentDescription depth_attachment{};
        depth_attachment.format = _depth_format;
        depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // clear depth to 1.0 (far plane) at the start of each frame
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // we don't need the depth data after rendering this frame
        depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        // ---------- Attachment references ----------
        // These tell a subpass WHICH attachment (by index into the array
        // passed to VkRenderPassCreateInfo below) to use, and in what
        // layout it should be while that subpass is executing.

        VkAttachmentReference color_attachment_ref{};
        color_attachment_ref.attachment = 0; // index 0 in the attachments array below
        color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depth_attachment_ref{};
        depth_attachment_ref.attachment = 1; // index 1 in the attachments array below
        depth_attachment_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        // ---------- Subpass ----------
        // A single subpass is enough for our needs - subpasses are mainly
        // useful for advanced techniques like deferred rendering, where
        // multiple rendering stages within the same render pass can share
        // attachments efficiently on tile-based mobile GPUs.
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_attachment_ref;
        subpass.pDepthStencilAttachment = &depth_attachment_ref;

        // ---------- Subpass dependency ----------
        // This is synchronization glue: it tells Vulkan when it's safe to
        // start writing to the color/depth attachments. VK_SUBPASS_EXTERNAL
        // means "whatever happened before this render pass began" (e.g.
        // the swapchain image being acquired). Without this dependency,
        // the GPU could start writing to the attachment before the image
        // is actually ready, causing a race condition.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0; // our only subpass, at index 0

        // Source scope: everything submitted before this render pass —
        // including the PREVIOUS FRAME. The depth image is shared by every
        // framebuffer, so the previous frame's depth writes are a genuine
        // write-after-write hazard against this frame's depth clear.
        //
        // LATE_FRAGMENT_TESTS is listed explicitly even though
        // COLOR_ATTACHMENT_OUTPUT already implies it (a stage mask covers
        // all logically earlier stages) — being explicit documents which
        // hazard this dependency actually closes.
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        // Those writes must be made AVAILABLE, not merely finished: an empty
        // srcAccessMask gives the execution dependency but performs no
        // availability operation for the write-after-write.
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        // ---------- Render pass creation ----------
        std::array<VkAttachmentDescription, 2> attachments = { color_attachment, depth_attachment };

        VkRenderPassCreateInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
        render_pass_info.pAttachments = attachments.data();
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies = &dependency;

        VkResult result = vkCreateRenderPass(device_handle, &render_pass_info, nullptr, &render_pass);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create render pass: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        std::cout << "[Vulkan_Render_Pass] Render pass created successfully (color + depth attachments).\n";
    }

    // ---------- Destructor ----------
    Vulkan_Render_Pass::~Vulkan_Render_Pass() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Render_Pass::Destroy() {
        if (render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_handle, render_pass, nullptr);
            render_pass = VK_NULL_HANDLE;
        }
    }

    // ---------- Move constructor ----------
    Vulkan_Render_Pass::Vulkan_Render_Pass(Vulkan_Render_Pass&& _other) noexcept
        : device_handle(_other.device_handle),
        render_pass(_other.render_pass),
        depth_format(_other.depth_format) {

        _other.render_pass = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Render_Pass& Vulkan_Render_Pass::operator=(Vulkan_Render_Pass&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            render_pass = _other.render_pass;
            depth_format = _other.depth_format;

            _other.render_pass = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Get_handle ----------
    VkRenderPass Vulkan_Render_Pass::Get_handle() const {
        assert(render_pass != VK_NULL_HANDLE && "Get_handle() called on a moved-from or destroyed Vulkan_Render_Pass");
        return render_pass;
    }

    // ---------- Get_depth_format ----------
    VkFormat Vulkan_Render_Pass::Get_depth_format() const {
        assert(render_pass != VK_NULL_HANDLE && "Get_depth_format() called on a moved-from or destroyed Vulkan_Render_Pass");
        return depth_format;
    }

}