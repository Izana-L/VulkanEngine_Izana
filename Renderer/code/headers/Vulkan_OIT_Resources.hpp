#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Image_Utils.hpp>

#include <vk_mem_alloc.h>

namespace Renderer_System
{

    // Vulkan_OIT_Resources: the two screen-sized targets of the weighted
    // blended order-independent transparency (McGuire and Bavoil, JCGT
    // 2013), written by the transparent subpass and read by the composite
    // subpass of Vulkan_Render_Pass:
    //
    //   accumulation (ACCUMULATION_FORMAT) - cleared to 0; every
    //       transparent fragment adds (color * alpha * w, alpha * w), with
    //       w the weight of mesh_oit.frag (additive blending ONE, ONE).
    //   revealage (REVEALAGE_FORMAT)       - cleared to 1; every
    //       transparent fragment multiplies it by (1 - alpha) (blending
    //       ZERO, ONE_MINUS_SRC_COLOR). It ends holding how much of the
    //       background stays visible.
    //
    // The composite divides accumulation.rgb by accumulation.a (the
    // weighted average color of the layers) and blends it "over" the
    // opaque color with opacity 1 - revealage.
    //
    // Both live only inside the render pass: cleared on load, not stored,
    // read as input attachments by the composite subpass. Their usage is
    // COLOR_ATTACHMENT | INPUT_ATTACHMENT | TRANSIENT_ATTACHMENT, and their
    // memory is lazily allocated where the device offers it (tile-based
    // GPUs keep them on chip), device-local otherwise.
    //
    // Like the depth buffer, one pair serves every framebuffer and both
    // frames in flight: the external dependency of the transparent subpass
    // (Vulkan_Render_Pass) orders their reuse after the previous frame.
    //
    // Memory at 1280x720: 7.4 MB of accumulation (8 bytes per pixel) and
    // 1.8 MB of revealage (2 bytes per pixel).
    //
    // Not copyable; movable, like Vulkan_Depth_Resources.
    class Vulkan_OIT_Resources
    {
    public:

        // Half-precision float RGBA: premultiplied, weighted colors and
        // weights, summed over every layer of a pixel. The weight function
        // of mesh_oit.frag is bounded so that the sum stays below 65504,
        // the largest finite half-precision value. Color attachment and
        // blending support are mandatory for this format.
        static constexpr VkFormat ACCUMULATION_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

        // Half-precision float, one channel: the product of (1 - alpha).
        // R8_UNORM would halve the memory, but rounds every step of the
        // product to 1/255, which becomes visible after a few layers of
        // low alpha. Color attachment and blending support are mandatory.
        static constexpr VkFormat REVEALAGE_FORMAT = VK_FORMAT_R16_SFLOAT;

        // Creates both images and their views, sized to _extent.
        // Throws std::runtime_error if a format lacks color attachment or
        // blending support (never, on a conformant device).
        Vulkan_OIT_Resources(const Vulkan_Device& _device, VmaAllocator _allocator, VkExtent2D _extent);

        ~Vulkan_OIT_Resources();

        Vulkan_OIT_Resources(const Vulkan_OIT_Resources&) = delete;
        Vulkan_OIT_Resources& operator=(const Vulkan_OIT_Resources&) = delete;

        Vulkan_OIT_Resources(Vulkan_OIT_Resources&& _other) noexcept;
        Vulkan_OIT_Resources& operator=(Vulkan_OIT_Resources&& _other) noexcept;

        // Recreates both targets at _new_extent. Called together with the
        // swapchain: the targets must match the framebuffer extent exactly.
        // Precondition: no command buffer pending execution uses them
        // (after the idle wait of Renderer::Recreate_swapchain). The views
        // change: framebuffers and descriptors that reference them are
        // rebuilt by the caller.
        void Recreate(VkExtent2D _new_extent);

        VkImageView Get_accumulation_view() const;
        VkImageView Get_revealage_view() const;
        VkImage     Get_accumulation_image() const;
        VkImage     Get_revealage_image() const;

    private:

        // One target: image, allocation and view.
        struct Target
        {
            Vulkan_Image_Utils::Image_Allocation image;
            VkImageView                          view = VK_NULL_HANDLE;
        };

        // Creates both targets at _extent. Shared by the constructor and
        // Recreate.
        void Create(VkExtent2D _extent);

        // Creates one target of _format. Lazily allocated memory first,
        // then dedicated device-local memory when no memory type is lazily
        // allocated (desktop GPUs).
        Target Create_target(VkExtent2D _extent, VkFormat _format, bool& _out_lazily_allocated) const;

        // Destroys the views, then the images. Safe on a partially created
        // or moved-from object.
        void Destroy();
        static void Destroy_target(VkDevice _device, VmaAllocator _allocator, Target& _target);

        VkDevice     device_handle;
        VmaAllocator allocator;
        VkExtent2D   current_extent;

        Target       accumulation;
        Target       revealage;
    };

} // namespace Renderer_System
