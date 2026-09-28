#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Render_Pass.hpp>
#include <Vulkan_Swapchain.hpp>
#include <Vulkan_Depth_Resources.hpp>
#include <Vulkan_OIT_Resources.hpp>

#include <vector>

namespace Renderer_System 
{

    // Vulkan_Framebuffer: owns one VkFramebuffer per swapchain image,
    // each one binding together a specific color image view (from the
    // swapchain) with the shared depth image view (from
    // Vulkan_Depth_Resources) and the two shared OIT targets (from
    // Vulkan_OIT_Resources), following the attachment structure defined by
    // Vulkan_Render_Pass (Render_Pass_Attachment order).
    //
    // A framebuffer is what command buffers actually render INTO during
    // a render pass - it's the final piece connecting "the rules of how
    // to draw" (render pass) with "the actual images to draw on"
    // (swapchain images + depth buffer).
    class Vulkan_Framebuffer 
    {

        VkDevice device_handle;
        std::vector<VkFramebuffer> framebuffers;


    public:
        // Creates one framebuffer per swapchain image view, all sharing
        // the same depth image view and OIT targets and following the given
        // render pass's attachment layout.
        Vulkan_Framebuffer(const Vulkan_Device& _device,
            const Vulkan_Render_Pass& _render_pass,
            const Vulkan_Swapchain& _swapchain,
            const Vulkan_Depth_Resources& _depth_resources,
            const Vulkan_OIT_Resources& _oit_resources);
        
            

        ~Vulkan_Framebuffer();

        Vulkan_Framebuffer(const Vulkan_Framebuffer&) = delete;
        Vulkan_Framebuffer& operator=(const Vulkan_Framebuffer&) = delete;

        Vulkan_Framebuffer(Vulkan_Framebuffer&& _other) noexcept;
        Vulkan_Framebuffer& operator=(Vulkan_Framebuffer&& _other) noexcept;

        // Recreates all framebuffers - must be called whenever the
        // swapchain (and the depth and OIT targets) are recreated, since
        // the framebuffers reference specific image views that no longer
        // exist after a resize.
        void Recreate(const Vulkan_Render_Pass& _render_pass,
                      const Vulkan_Swapchain& _swapchain,
                      const Vulkan_Depth_Resources& _depth_resources,
                      const Vulkan_OIT_Resources& _oit_resources);
        
            

        // Returns the framebuffer corresponding to a specific swapchain
        // image index - this is what you pass to vkCmdBeginRenderPass
        // when recording a command buffer for that particular image.
        VkFramebuffer Get_framebuffer(uint32_t _image_index) const;

        // Total number of framebuffers (matches the swapchain's image count)
        uint32_t Get_count() const;

    private:
        // Destroys all framebuffers. Shared by destructor, move
        // assignment, and Recreate().
        void Destroy();

        // Creates one framebuffer per swapchain image view - the actual
        // work, shared by the constructor and Recreate().
        void Create(const Vulkan_Render_Pass& _render_pass,
                    const Vulkan_Swapchain& _swapchain,
                    const Vulkan_Depth_Resources& _depth_resources,
                    const Vulkan_OIT_Resources& _oit_resources);

            
      
    };

}