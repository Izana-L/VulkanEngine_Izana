#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <ImageData.hpp>

#include <vk_mem_alloc.h>

#include <cstdint>

namespace Renderer_System
{
    namespace Vulkan_Image_Utils
    {

        // An image and the VMA allocation backing it. Same idea as
        // Vulkan_Buffer_Utils::Buffer_Allocation: the two always travel
        // together, so they are freed together.
        struct Image_Allocation
        {
            VkImage       image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
        };

        // Creates a VkImage and sub-allocates its memory through VMA in one
        // call. Same "create resource, allocate, bind" pattern as
        // Vulkan_Buffer_Utils::Create_buffer, applied to images.
        //
        // _width / _height: image dimensions in texels
        // _mip_levels: number of mip levels to allocate space for. The image
        //   itself only contains mip 0 until Generate_mipmaps() fills the rest.
        // _format: pixel format (e.g. VK_FORMAT_R8G8B8A8_SRGB for color textures,
        //   VK_FORMAT_R8G8B8A8_UNORM for data textures like normal maps)
        // _tiling: almost always VK_IMAGE_TILING_OPTIMAL for GPU-sampled textures
        //   (VK_IMAGE_TILING_LINEAR is only needed for CPU-readable images)
        // _usage: what the image will be used for (e.g. TRANSFER_DST_BIT |
        //   SAMPLED_BIT for a standard texture, plus TRANSFER_SRC_BIT if mips
        //   will be generated via blit from this image to itself)
        //
        // The memory properties parameter is gone: every image created here
        // is DEVICE_LOCAL, which VMA_MEMORY_USAGE_AUTO derives from _usage.
        Image_Allocation Create_image(
            VmaAllocator      _allocator,
            uint32_t          _width,
            uint32_t          _height,
            uint32_t          _mip_levels,
            VkFormat          _format,
            VkImageTiling     _tiling,
            VkImageUsageFlags _usage,
            bool              _dedicated = false
        );

        // Frees the image and its allocation. Does NOT touch image views —
        // destroy those first. Safe with null handles.
        void Destroy_image(VmaAllocator _allocator, Image_Allocation& _image);

        // Creates a VkImageView for an existing VkImage. A VkImage is just
        // memory — the view tells Vulkan how to interpret it (format, which
        // mip levels and array layers are visible, 2D vs cubemap, etc.).
        //
        // _image: the image to create a view of
        // _format: must match (or be compatible with) the image's format
        // _aspect_flags: VK_IMAGE_ASPECT_COLOR_BIT for color textures,
        //   VK_IMAGE_ASPECT_DEPTH_BIT for depth images
        // _mip_levels: how many mip levels this view exposes, starting from 0
        VkImageView Create_image_view(
            VkDevice              _device,
            VkImage               _image,
            VkFormat              _format,
            VkImageAspectFlags    _aspect_flags,
            uint32_t              _mip_levels
        );

        // Records a pipeline barrier that transitions an image between layouts
        // and inserts the correct access masks and pipeline stages for the
        // transition. Only the transitions actually used by the texture upload
        // pipeline are supported — extend this function if new transitions
        // are needed rather than calling vkCmdPipelineBarrier directly elsewhere.
        //
        // Supported transitions:
        //   UNDEFINED            -> TRANSFER_DST_OPTIMAL   (before buffer copy)
        //   TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL (after copy, no mips)
        //   TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL   (before mip generation)
        //   TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL (after mip generation)
        //
        // _command_buffer: must already be in the recording state
        // _mip_levels: number of mip levels affected by this transition
        void Transition_image_layout(
            VkCommandBuffer _command_buffer,
            VkImage         _image,
            VkImageLayout   _old_layout,
            VkImageLayout   _new_layout,
            uint32_t        _mip_levels
        );

        // Copies the contents of a CPU-visible staging buffer into an image
        // that has already been transitioned to TRANSFER_DST_OPTIMAL. Copies
        // only mip level 0 — Generate_mipmaps() fills the remaining levels.
        //
        // _command_buffer: must already be in the recording state
        void Copy_buffer_to_image(
            VkCommandBuffer _command_buffer,
            VkBuffer        _buffer,
            VkImage         _image,
            uint32_t        _width,
            uint32_t        _height
        );

        // Generates all mip levels above 0 by repeatedly blitting each level
        // into a half-size version of itself (vkCmdBlitImage with linear
        // filtering). Leaves every mip level in SHADER_READ_ONLY_OPTIMAL layout
        // when done — no further transition needed after calling this.
        //
        // Precondition: the image must support VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
        // for the given format (checked via assert in debug builds) and must
        // already be in TRANSFER_DST_OPTIMAL layout (mip 0 freshly copied).
        //
        // _command_buffer: must already be in the recording state
        void Generate_mipmaps(
            const Vulkan_Device& _device,
            VkCommandBuffer      _command_buffer,
            VkImage              _image,
            VkFormat             _format,
            int32_t              _width,
            int32_t              _height,
            uint32_t             _mip_levels
        );

        // Computes how many mip levels a full mip chain needs for the given
        // dimensions: floor(log2(max(width, height))) + 1.
        // A 1024x1024 texture needs 11 levels (1024, 512, ..., 1).
        uint32_t Compute_mip_levels(uint32_t _width, uint32_t _height);

        // Maps the CPU-side pixel format of ImageData to the VkFormat the
        // GPU image is created with. This is the ONE place where
        // CoreTypes::Pixel_Format meets VkFormat, as ImageData.hpp promises.
        VkFormat To_vk_format(CoreTypes::Pixel_Format _format);

        // Bytes of one texel for the uncompressed formats Texture_GPU can
        // upload with a tightly packed buffer copy. Returns 0 for
        // block-compressed or unsupported formats, which Texture_GPU
        // rejects (a buffer-to-image copy of a block format needs
        // block-sized regions and mip generation by blit is not possible).
        uint32_t Bytes_per_pixel(VkFormat _format);

    } // namespace Vulkan_Image_Utils
} // namespace Renderer