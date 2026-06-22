#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

namespace Renderer {

    // Vulkan_Render_Pass: owns the VkRenderPass, which describes the
    // STRUCTURE of a rendering operation - what attachments (color,
    // depth) are used, what state they start/end in, and how subpasses
    // are organized. It does NOT draw anything itself; it's the contract
    // that Vulkan_Pipeline and Vulkan_Framebuffer are built against, and
    // that command buffers follow when recording draw commands.
    //
    // Includes a depth attachment from the start, even though the first
    // triangle doesn't strictly need it - any real 3D scene with multiple
    // overlapping objects requires depth testing, and changing the render
    // pass later would force rebuilding the pipeline and framebuffers too.
    class Vulkan_Render_Pass 
    {
        VkDevice device_handle;
        VkRenderPass render_pass;
        VkFormat depth_format;

    public:
        // Creates a render pass with one color attachment (matching the
        // swapchain's format) and one depth attachment.
        // _depth_format should be a format supported by the GPU for depth
        // attachments (queried via Vulkan_Device in a helper added below,
        // or passed in directly once that querying exists).
        Vulkan_Render_Pass(
            const Vulkan_Device& _device,
            VkFormat _color_format,
            VkFormat _depth_format
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