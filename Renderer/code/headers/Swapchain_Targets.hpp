#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Swapchain.hpp>
#include <Vulkan_Render_Pass.hpp>
#include <Vulkan_Depth_Resources.hpp>
#include <Vulkan_OIT_Resources.hpp>
#include <Vulkan_Framebuffer.hpp>

#include <utility>

namespace Renderer_System
{

    // Swapchain_Targets: everything that is sized by the swapchain and
    // attached to the render pass through the framebuffers: the depth buffer,
    // the two OIT targets and one framebuffer per swapchain image.
    //
    // The one place that builds them. The Renderer's constructor and every
    // swapchain recreation (Renderer::Recreate_swapchain) construct a
    // Swapchain_Targets from the same arguments, so the depth buffer always
    // takes its format from the render pass it is attached through, and the
    // framebuffers check the swapchain format against it (Vulkan_Framebuffer).
    //
    // Replacing the set is a move assignment, which cannot throw: the new
    // set is built completely first, in a temporary, and swapped in only when
    // every object exists, so a failed recreation leaves the current set
    // untouched. The framebuffers are released first, since they refer to
    // the views of the other two.
    //
    // Members are declared in dependency order (the framebuffers reference
    // the other two), so the destructor releases the framebuffers first too.
    struct Swapchain_Targets
    {
        Vulkan_Depth_Resources depth;
        Vulkan_OIT_Resources   oit;
        Vulkan_Framebuffer     framebuffers;

        // Builds the targets for the current images of _swapchain. Throws
        // like its members do (Vulkan_Error, std::runtime_error when a
        // format does not match the render pass); everything created before
        // the failure is released.
        Swapchain_Targets(const Vulkan_Device& _device, VmaAllocator _allocator,
                          const Vulkan_Render_Pass& _render_pass, const Vulkan_Swapchain& _swapchain)
            : depth(_device, _allocator, _render_pass.Get_depth_format(), _swapchain.Get_extent()),
            oit(_device, _allocator, _swapchain.Get_extent()),
            framebuffers(_device, _render_pass, _swapchain, depth, oit)
        {
        }

        Swapchain_Targets(const Swapchain_Targets&) = delete;
        Swapchain_Targets& operator=(const Swapchain_Targets&) = delete;

        Swapchain_Targets(Swapchain_Targets&& _other) noexcept = default;

        // Replaces this set with _other's; _other is left empty.
        Swapchain_Targets& operator=(Swapchain_Targets&& _other) noexcept
        {
            if (this != &_other)
            {
                framebuffers = std::move(_other.framebuffers);
                oit = std::move(_other.oit);
                depth = std::move(_other.depth);
            }

            return *this;
        }
    };

} // namespace Renderer_System
