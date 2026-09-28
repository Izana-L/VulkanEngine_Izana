#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <cstdint>

namespace Renderer_System {

    // Attachment indices of the render pass. Vulkan_Framebuffer binds the
    // views in this order and Renderer::Record_command_buffer fills the
    // clear values in this order.
    namespace Render_Pass_Attachment
    {
        inline constexpr uint32_t Color = 0;              // swapchain image
        inline constexpr uint32_t Depth = 1;              // shared depth buffer
        inline constexpr uint32_t Oit_Accumulation = 2;   // Vulkan_OIT_Resources, cleared to 0
        inline constexpr uint32_t Oit_Revealage = 3;      // Vulkan_OIT_Resources, cleared to 1
        inline constexpr uint32_t Count = 4;
    }

    // Subpass indices. Every graphics pipeline is built for one of them
    // (Pipeline_Config::subpass), and Renderer::Record_command_buffer moves
    // through them with vkCmdNextSubpass.
    namespace Render_Subpass
    {
        // Color = swapchain image, depth written. Opaque draws.
        inline constexpr uint32_t Opaque = 0;

        // Colors = accumulation and revealage (two attachments, independent
        // blending), depth read-only for the test. The swapchain image is
        // preserved. Transparent draws, in any order.
        inline constexpr uint32_t Transparent = 1;

        // Inputs = accumulation and revealage (input attachments), color =
        // swapchain image, depth read-only. The OIT composite, then the
        // debug bounding volumes, which test against the opaque depth.
        inline constexpr uint32_t Composite = 2;

        inline constexpr uint32_t Count = 3;
    }

    // Vulkan_Render_Pass: owns the VkRenderPass, which describes the
    // STRUCTURE of a rendering operation - what attachments are used, what
    // state they start/end in, and how subpasses are organized. It does
    // NOT draw anything itself; it's the contract that Vulkan_Pipeline and
    // Vulkan_Framebuffer are built against, and that command buffers follow
    // when recording draw commands.
    //
    // Four attachments (Render_Pass_Attachment) and three subpasses
    // (Render_Subpass) implementing weighted blended order-independent
    // transparency:
    //
    //   0 opaque      writes color and depth;
    //   1 transparent accumulates every transparent fragment into the two
    //                 OIT targets, testing against the opaque depth;
    //   2 composite   reads the OIT targets as input attachments at the
    //                 same pixel and blends the result over the color of
    //                 subpass 0.
    //
    // Subpasses rather than separate render passes: the OIT targets are
    // read at the pixel that wrote them, which input attachments express
    // without barriers between passes, and they never need to be stored to
    // memory (loadOp CLEAR, storeOp DONT_CARE; on tile-based GPUs they can
    // stay on chip).
    //
    // Dependencies (by region where both sides are framebuffer-space):
    //   EXTERNAL -> 0  color and depth of the previous frame (the depth
    //                  buffer is shared by every framebuffer) before this
    //                  frame's clears; also where the wait for the acquired
    //                  swapchain image lands.
    //   EXTERNAL -> 1  the OIT targets are shared by the frames in flight as
    //                  well: the previous frame's composite reads and
    //                  accumulation writes before this frame's clears and
    //                  layout transitions. Without it the implicit external
    //                  dependency would not wait for anything.
    //   0 -> 1         depth writes before the depth test of the
    //                  transparent fragments.
    //   1 -> 2         accumulation writes before their input attachment
    //                  reads; with 0 -> 1, the layout transitions of the
    //                  depth buffer and of the OIT targets before their
    //                  store operations at the end of subpass 2 (DONT_CARE
    //                  counts as a write).
    //   0 -> 2         color writes before the composite blends over them,
    //                  and depth writes before the test of the debug view.
    class Vulkan_Render_Pass
    {
        VkDevice device_handle;
        VkRenderPass render_pass;
        VkFormat depth_format;

    public:
        // _color_format: the swapchain format. _depth_format: a depth
        // format supported for depth attachments
        // (Vulkan_Device::Find_supported_depth_format). _accumulation_format
        // and _revealage_format: the formats of the OIT targets
        // (Vulkan_OIT_Resources), which must support color attachment and
        // blending.
        Vulkan_Render_Pass(
            const Vulkan_Device& _device,
            VkFormat _color_format,
            VkFormat _depth_format,
            VkFormat _accumulation_format,
            VkFormat _revealage_format
        );

        ~Vulkan_Render_Pass();

        Vulkan_Render_Pass(const Vulkan_Render_Pass&) = delete;
        Vulkan_Render_Pass& operator=(const Vulkan_Render_Pass&) = delete;

        Vulkan_Render_Pass(Vulkan_Render_Pass&& _other) noexcept;
        Vulkan_Render_Pass& operator=(Vulkan_Render_Pass&& _other) noexcept;

        // Raw handle, needed by Vulkan_Pipeline (to build a pipeline
        // compatible with this render pass) and Vulkan_Framebuffer (to
        // create framebuffers that match this render pass's attachments).
        VkRenderPass Get_handle() const;

        // The depth format this render pass was created with - needed by
        // whoever creates the actual depth image/view to make sure it
        // matches what this render pass expects.
        VkFormat Get_depth_format() const;

    private:
        // Destroys the VkRenderPass. Shared by destructor and move assignment.
        void Destroy();


    };

}
