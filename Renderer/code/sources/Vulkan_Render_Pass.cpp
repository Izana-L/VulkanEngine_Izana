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
        VkFormat _depth_format,
        VkFormat _accumulation_format,
        VkFormat _revealage_format)

        : render_pass(),
        depth_format(_depth_format) {

        const VkDevice device_handle = _device.Get_logical_device_handle();

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a render pass");
        assert(_color_format != VK_FORMAT_UNDEFINED && "Color format cannot be VK_FORMAT_UNDEFINED");
        assert(_depth_format != VK_FORMAT_UNDEFINED && "Depth format cannot be VK_FORMAT_UNDEFINED");
        assert(_accumulation_format != VK_FORMAT_UNDEFINED && "Accumulation format cannot be VK_FORMAT_UNDEFINED");
        assert(_revealage_format != VK_FORMAT_UNDEFINED && "Revealage format cannot be VK_FORMAT_UNDEFINED");

        std::array<VkAttachmentDescription, Render_Pass_Attachment::Count> attachments{};

        // ---------- Color attachment ----------
        // Describes the swapchain image we'll be drawing color into.
        VkAttachmentDescription& color_attachment = attachments[Render_Pass_Attachment::Color];
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
        // overlap on screen). Written by the opaque subpass only; the
        // transparent and composite subpasses use it read-only, for the
        // test.
        VkAttachmentDescription& depth_attachment = attachments[Render_Pass_Attachment::Depth];
        depth_attachment.format = _depth_format;
        depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;// Reverse-Z: cleared to 0.0 (the far end) each frame - value lives in Renderer::Record_command_buffer
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // we don't need the depth data after rendering this frame
        depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // The layout of its last use: no transition at the end of the pass.
        depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        // ---------- OIT attachments ----------
        // Accumulation and revealage (Vulkan_OIT_Resources). They only live
        // inside the render pass: cleared when the transparent subpass
        // first uses them (to 0 and 1, values in
        // Renderer::Record_command_buffer), read by the composite subpass,
        // never stored. The final layout is the one of their last use.
        const auto describe_oit_target = [](VkAttachmentDescription& _attachment, VkFormat _format)
            {
                _attachment.format = _format;
                _attachment.samples = VK_SAMPLE_COUNT_1_BIT;
                _attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                _attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                _attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                _attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                _attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                _attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            };

        describe_oit_target(attachments[Render_Pass_Attachment::Oit_Accumulation], _accumulation_format);
        describe_oit_target(attachments[Render_Pass_Attachment::Oit_Revealage], _revealage_format);

        // ---------- Attachment references ----------
        // These tell a subpass WHICH attachment (by index into the array
        // above) to use, and in what layout it should be while that
        // subpass is executing. A layout change between two subpasses is
        // performed by the render pass itself, inside the dependency
        // between them.

        const VkAttachmentReference color_reference{ Render_Pass_Attachment::Color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        const VkAttachmentReference depth_write_reference{ Render_Pass_Attachment::Depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        // Read-only depth: the transparent and composite subpasses test
        // against it and never write it (their draws disable depth writes,
        // which a read-only layout requires).
        const VkAttachmentReference depth_read_reference{ Render_Pass_Attachment::Depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };

        // Outputs of the transparent subpass, in fragment output location
        // order: location 0 = accumulation, location 1 = revealage
        // (mesh_oit.frag).
        const std::array<VkAttachmentReference, 2> oit_outputs = { {
            { Render_Pass_Attachment::Oit_Accumulation, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { Render_Pass_Attachment::Oit_Revealage,    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } } };

        // Inputs of the composite subpass, in input_attachment_index order:
        // 0 = accumulation, 1 = revealage (oit_composite.frag).
        const std::array<VkAttachmentReference, 2> oit_inputs = { {
            { Render_Pass_Attachment::Oit_Accumulation, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { Render_Pass_Attachment::Oit_Revealage,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } } };

        // Written by subpass 0 and blended over by subpass 2: its contents
        // must survive subpass 1, which does not use it.
        const uint32_t preserved_color = Render_Pass_Attachment::Color;

        // ---------- Subpasses ----------
        std::array<VkSubpassDescription, Render_Subpass::Count> subpasses{};

        VkSubpassDescription& opaque = subpasses[Render_Subpass::Opaque];
        opaque.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        opaque.colorAttachmentCount = 1;
        opaque.pColorAttachments = &color_reference;
        opaque.pDepthStencilAttachment = &depth_write_reference;

        VkSubpassDescription& transparent = subpasses[Render_Subpass::Transparent];
        transparent.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        transparent.colorAttachmentCount = static_cast<uint32_t>(oit_outputs.size());
        transparent.pColorAttachments = oit_outputs.data();
        transparent.pDepthStencilAttachment = &depth_read_reference;
        transparent.preserveAttachmentCount = 1;
        transparent.pPreserveAttachments = &preserved_color;

        VkSubpassDescription& composite = subpasses[Render_Subpass::Composite];
        composite.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        composite.inputAttachmentCount = static_cast<uint32_t>(oit_inputs.size());
        composite.pInputAttachments = oit_inputs.data();
        composite.colorAttachmentCount = 1;
        composite.pColorAttachments = &color_reference;
        composite.pDepthStencilAttachment = &depth_read_reference;

        // ---------- Subpass dependencies ----------
        std::array<VkSubpassDependency, 5> dependencies{};

        // EXTERNAL -> opaque. VK_SUBPASS_EXTERNAL means "whatever happened
        // before this render pass began", the previous frame included.
        // Without this dependency, the GPU could start writing to the
        // attachments before the swapchain image is ready (the submit waits
        // for it at COLOR_ATTACHMENT_OUTPUT) or while the previous frame
        // still uses the shared depth buffer.
        //
        // Source scope: the depth image is shared by every framebuffer, so
        // the previous frame's depth writes are a genuine write-after-write
        // hazard against this frame's depth clear, and its depth tests a
        // write-after-read one. LATE_FRAGMENT_TESTS is listed explicitly
        // even though COLOR_ATTACHMENT_OUTPUT already implies it (a source
        // stage mask covers all logically earlier stages): being explicit
        // documents which hazard this dependency closes. The writes must be
        // made AVAILABLE, not merely finished: an empty srcAccessMask gives
        // the execution dependency but no availability operation for the
        // write-after-write.
        VkSubpassDependency& external_to_opaque = dependencies[0];
        external_to_opaque.srcSubpass = VK_SUBPASS_EXTERNAL;
        external_to_opaque.dstSubpass = Render_Subpass::Opaque;
        external_to_opaque.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        external_to_opaque.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        external_to_opaque.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        external_to_opaque.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        // EXTERNAL -> transparent. The OIT targets are shared by the frames
        // in flight, like the depth buffer, and their first use is this
        // subpass: without an explicit dependency, the implicit one would
        // have TOP_OF_PIPE as source and the clear and layout transition of
        // this frame could overlap the previous frame's use.
        //   - previous composite reads (input attachments, FRAGMENT_SHADER):
        //     write-after-read, an execution dependency;
        //   - previous accumulation writes (COLOR_ATTACHMENT_OUTPUT):
        //     write-after-write, made available here.
        // Destination: the clear and the blending of this subpass.
        VkSubpassDependency& external_to_transparent = dependencies[1];
        external_to_transparent.srcSubpass = VK_SUBPASS_EXTERNAL;
        external_to_transparent.dstSubpass = Render_Subpass::Transparent;
        external_to_transparent.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        external_to_transparent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        external_to_transparent.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        external_to_transparent.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Opaque -> transparent: the depth written by the opaque draws
        // before the depth test of the transparent fragments. The
        // transition of the depth buffer to its read-only layout happens
        // inside this dependency; its destination also covers depth
        // writes, which this subpass never performs, so that the chain
        // continued by transparent -> composite orders the transition
        // before the store operation of the depth buffer (DONT_CARE counts
        // as a write) at the end of the composite subpass.
        VkSubpassDependency& opaque_to_transparent = dependencies[2];
        opaque_to_transparent.srcSubpass = Render_Subpass::Opaque;
        opaque_to_transparent.dstSubpass = Render_Subpass::Transparent;
        opaque_to_transparent.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        opaque_to_transparent.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        opaque_to_transparent.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        opaque_to_transparent.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        opaque_to_transparent.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        // Transparent -> composite: the accumulation and revealage writes
        // before their reads as input attachments, at the same pixel. The
        // transition of both targets to SHADER_READ_ONLY_OPTIMAL happens
        // inside this dependency. The last use of the depth buffer and of
        // the OIT targets is the composite subpass, where their store
        // operations (DONT_CARE, a write for synchronization purposes)
        // take place: the destination also covers those writes, after the
        // layout transitions and the writes of this subpass, and the source
        // includes the fragment test stages so the chain started by
        // opaque -> transparent reaches them.
        VkSubpassDependency& transparent_to_composite = dependencies[3];
        transparent_to_composite.srcSubpass = Render_Subpass::Transparent;
        transparent_to_composite.dstSubpass = Render_Subpass::Composite;
        transparent_to_composite.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        transparent_to_composite.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        transparent_to_composite.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        transparent_to_composite.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        transparent_to_composite.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        // Opaque -> composite: the swapchain color written by the opaque
        // draws before the composite blends over it (read and write), and
        // the opaque depth before the test of the debug bounding volumes.
        // The depth buffer changes layout between these two subpasses as
        // well (the transition away from subpass 0's layout follows the
        // availability operations of every dependency leaving subpass 0,
        // this one included), so the destination also covers the store
        // operation of the depth buffer at the end of subpass 2.
        VkSubpassDependency& opaque_to_composite = dependencies[4];
        opaque_to_composite.srcSubpass = Render_Subpass::Opaque;
        opaque_to_composite.dstSubpass = Render_Subpass::Composite;
        opaque_to_composite.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        opaque_to_composite.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        opaque_to_composite.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        opaque_to_composite.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        opaque_to_composite.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        // ---------- Render pass creation ----------
        VkRenderPassCreateInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
        render_pass_info.pAttachments = attachments.data();
        render_pass_info.subpassCount = static_cast<uint32_t>(subpasses.size());
        render_pass_info.pSubpasses = subpasses.data();
        render_pass_info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        render_pass_info.pDependencies = dependencies.data();

        // The output of a failed creation is undefined: the owner is built
        // only from the handle of a successful one.
        VkRenderPass created = VK_NULL_HANDLE;
        VK_CHECK(vkCreateRenderPass(device_handle, &render_pass_info, nullptr, &created),
            "Failed to create render pass");

        render_pass = Unique_Render_Pass(device_handle, created);

        std::cout << "[Vulkan_Render_Pass] Render pass created successfully (color, depth and two OIT attachments; "
                     "opaque, transparent and composite subpasses).\n";
    }

    // ---------- Get_handle ----------
    VkRenderPass Vulkan_Render_Pass::Get_handle() const {
        assert(render_pass && "Get_handle() called on a moved-from or destroyed Vulkan_Render_Pass");
        return render_pass.Get();
    }

    // ---------- Get_depth_format ----------
    VkFormat Vulkan_Render_Pass::Get_depth_format() const {
        assert(render_pass && "Get_depth_format() called on a moved-from or destroyed Vulkan_Render_Pass");
        return depth_format;
    }

}