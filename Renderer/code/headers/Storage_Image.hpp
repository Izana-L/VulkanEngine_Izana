#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Image_Utils.hpp>

#include <vk_mem_alloc.h>

#include <cstdint>

namespace Renderer_System
{

    // Storage_Image: a 2D image written by compute shaders and read by any
    // shader through the bindless set. Owns the VkImage, its VMA
    // allocation and a single VkImageView, used both as the storage image
    // a compute pass writes (layout GENERAL) and as the sampled image a
    // bindless slot reads (layout SHADER_READ_ONLY_OPTIMAL).
    //
    // Knows nothing about the bindless set: the owner registers
    // Get_image_view() through Bindless_Registry::Register_texture and
    // keeps the slot. From then on the image follows the declared layout
    // rule documented there, through the two calls that bracket every
    // compute write:
    //
    //   Begin_write(cmd);   // UNDEFINED -> GENERAL
    //   ... dispatch that writes every texel ...
    //   End_write(cmd);     // GENERAL -> SHADER_READ_ONLY_OPTIMAL
    //
    // Rules for the owner:
    //   - Both calls are recorded before vkCmdBeginRenderPass. A barrier
    //     inside the current render pass would need a subpass
    //     self-dependency that the render pass does not declare.
    //   - Every Begin_write is followed by End_write in the same command
    //     buffer, before any command that may read the bindless set.
    //   - Begin_write discards the contents, so the dispatch writes every
    //     texel.
    //   - Once registered, the write is recorded every frame, or the image
    //     is left in SHADER_READ_ONLY_OPTIMAL when it stops being written.
    //     A frame that reads the slot while the image is still UNDEFINED
    //     (never written) breaks the declared layout rule.
    //
    // The layout is tracked on the CPU in recording order, only to assert
    // that Begin_write and End_write alternate; it is not the layout the
    // GPU sees at any given moment.
    //
    // One mip level: a compute write produces a single level, and a mip
    // chain would need a separate generation pass.
    //
    // Not copyable; movable, like Vulkan_Depth_Resources.
    class Storage_Image
    {
    public:
        // Creates a _width x _height image of _format with usage
        // STORAGE | SAMPLED, optimal tiling, and its view.
        //
        // Throws std::invalid_argument if a dimension is zero, and
        // std::runtime_error if the device does not support _format with
        // optimal tiling as both a storage image and a sampled image.
        // VK_FORMAT_R8G8B8A8_UNORM is guaranteed by the specification to
        // support both; sRGB formats rarely support storage.
        Storage_Image(const Vulkan_Device& _device,
            VmaAllocator         _allocator,
            uint32_t             _width,
            uint32_t             _height,
            VkFormat             _format);

        ~Storage_Image();

        Storage_Image(const Storage_Image&) = delete;
        Storage_Image& operator=(const Storage_Image&) = delete;

        Storage_Image(Storage_Image&& _other) noexcept;
        Storage_Image& operator=(Storage_Image&& _other) noexcept;

        // Records UNDEFINED -> GENERAL through
        // Vulkan_Image_Utils::Transition_image_layout: waits for the
        // bindless readers of previous frames, then allows compute writes.
        // Expects the image never written, or back in its declared layout
        // after the previous End_write.
        void Begin_write(VkCommandBuffer _command_buffer);

        // Records GENERAL -> SHADER_READ_ONLY_OPTIMAL: makes the compute
        // writes visible to every bindless reader stage and returns the
        // image to its declared layout. Expects a Begin_write before it.
        void End_write(VkCommandBuffer _command_buffer);

        VkImage     Get_image() const;
        VkImageView Get_image_view() const;
        VkFormat    Get_format() const;
        VkExtent2D  Get_extent() const;

        // Layout left by the last recorded Begin_write / End_write;
        // UNDEFINED before the first one. Recording order, not execution
        // order.
        VkImageLayout Get_recorded_layout() const;

    private:
        // Destroys the view, then the image and its allocation. Safe on a
        // partially created or moved-from object. Shared by the
        // destructor, move assignment and the constructor's failure path.
        void Destroy();

        VkDevice     device_handle;
        VmaAllocator allocator;
        VkFormat     format;
        VkExtent2D   extent;

        Vulkan_Image_Utils::Image_Allocation image;
        VkImageView                          image_view;

        VkImageLayout recorded_layout;
    };

}