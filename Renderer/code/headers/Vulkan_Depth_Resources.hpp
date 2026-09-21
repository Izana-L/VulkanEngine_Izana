#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Image_Utils.hpp>

namespace Renderer_System 
{

    // Vulkan_Depth_Resources: owns the depth buffer used for depth testing
    // (so closer objects correctly occlude farther ones when geometry
    // overlaps on screen). Manages the VkImage, its VMA allocation,
    // and the VkImageView that Vulkan_Framebuffer will reference.
    //
    // Unlike swapchain color images (one per swapchain image, since they
    // rotate), only ONE depth buffer is needed and shared across all
    // framebuffers - depth data from a previous frame doesn't need to be
    // preserved once that frame is done.
    class Vulkan_Depth_Resources 
    {
    public:
        // Creates the depth image, allocates its memory, and creates the
        // image view, sized to match the swapchain's current extent.
        Vulkan_Depth_Resources(
            const Vulkan_Device& _device,
            VmaAllocator _allocator,
            VkFormat _depth_format,
            VkExtent2D _extent
        );

        ~Vulkan_Depth_Resources();

        Vulkan_Depth_Resources(const Vulkan_Depth_Resources&) = delete;
        Vulkan_Depth_Resources& operator=(const Vulkan_Depth_Resources&) = delete;

        Vulkan_Depth_Resources(Vulkan_Depth_Resources&& _other) noexcept;
        Vulkan_Depth_Resources& operator=(Vulkan_Depth_Resources&& _other) noexcept;

        // Recreates the depth image/memory/view at a new size. Must be
        // called whenever the swapchain is recreated (e.g. after a window
        // resize), since the depth buffer must always match the
        // swapchain's current extent exactly.
        void Recreate(VkExtent2D _new_extent);

        // The image view, needed by Vulkan_Framebuffer to attach this
        // depth buffer to each framebuffer it creates.
        VkImageView Get_image_view() const;

        // The format this depth buffer was created with.
        VkFormat Get_format() const;

    private:
        // Destroys the image view, frees the memory, and destroys the
        // image, in that order (reverse of creation). Shared by the
        // destructor, move assignment, and Recreate().
        void Destroy();

        // Creates the VkImage with its memory, and creates the
        // VkImageView - the actual work, shared by the constructor
        // and Recreate().
        void Create(VkExtent2D _extent);

        VkDevice device_handle;

        // Replaces the Vulkan_Device back-pointer, which only existed to
        // call Find_memory_type() again inside Recreate().
        VmaAllocator allocator;

        VkFormat depth_format;
        VkExtent2D current_extent;

        Vulkan_Image_Utils::Image_Allocation depth_image;
        VkImageView depth_image_view;
    };

}