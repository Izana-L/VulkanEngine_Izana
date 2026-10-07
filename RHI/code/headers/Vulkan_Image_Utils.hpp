#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Barrier.hpp>
#include <Vulkan_Device.hpp>
#include <ImageData.hpp>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <span>

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
        //
        // Checked in every build, before anything is created: throws
        // std::invalid_argument if a dimension or _mip_levels is zero, and
        // std::runtime_error if the device cannot create an image of this
        // size, format, tiling and usage (Require_image_support).
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

        // Throws unless the physical device of _allocator can create a 2D
        // image of _width x _height texels, _mip_levels levels, _format,
        // _tiling and _usage (one layer, one sample, no create flags, which
        // is every image of the engine). Asked of the device
        // (vkGetPhysicalDeviceImageFormatProperties), not assumed: the
        // largest extent and the most mip levels an image can have depend on
        // the format, the tiling and the usage as well as on
        // maxImageDimension2D, and creating an image beyond them is invalid
        // usage that a release build, without validation layers, would hand
        // to the driver. The total size of the image (maxResourceSize) is
        // not checked: an allocation too large for the device fails when the
        // memory is allocated, with an error result.
        //   std::invalid_argument - a dimension or _mip_levels is zero;
        //   std::runtime_error    - the combination is not supported, or an
        //                           extent or the level count is above what
        //                           the device allows (the message gives
        //                           both values).
        // Create_image calls it; whoever creates an image by another route
        // (vmaCreateImage with its own flags, as the OIT targets do) calls
        // it first. _caller starts the message.
        void Require_image_support(
            VmaAllocator      _allocator,
            VkFormat          _format,
            VkImageTiling     _tiling,
            VkImageUsageFlags _usage,
            uint32_t          _width,
            uint32_t          _height,
            uint32_t          _mip_levels,
            const char*       _caller
        );

        // Format features, with optimal tiling, that every image registered
        // in the bindless texture array needs. The sampler is chosen per
        // draw by the material, not by the texture, so any slot may be read
        // with any CoreTypes::Sampler_Preset, the Linear_* ones included,
        // and reading an image through a linear sampler requires
        // SAMPLED_IMAGE_FILTER_LINEAR_BIT on its format. The 8-bit UNORM
        // and SRGB color formats and R16G16B16A16_SFLOAT have both
        // features guaranteed by the specification; the 32-bit float
        // formats (R32_SFLOAT, R32G32B32A32_SFLOAT...) do not.
        inline constexpr VkFormatFeatureFlags Bindless_Sampled_Format_Features =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

        // Throws std::runtime_error unless _format supports every feature in
        // _required with optimal tiling on the physical device of _device.
        // The message starts with _caller and lists the missing features.
        // Enforced in every build: creating or using an image beyond the
        // features of its format is invalid usage, and nothing guarantees
        // a validation layer is present to report it.
        void Require_optimal_tiling_features(
            const Vulkan_Device& _device,
            VkFormat             _format,
            VkFormatFeatureFlags _required,
            const char*          _caller
        );

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

        // Records the barrier of one step of the texture upload in
        // Texture_GPU. Only the transitions listed below are supported; any
        // other pair throws std::runtime_error. Each of them involves a
        // TRANSFER_* layout, which only transfer commands use, so the pair
        // identifies the transfer side; the other side is fixed by the
        // upload flow and stated next to each pair. Any other transition,
        // GENERAL above all, is recorded with
        // Vulkan_Barrier::Record_image_barrier and explicit scopes instead
        // of being added here.
        //
        // Supported transitions:
        //     UNDEFINED            -> TRANSFER_DST_OPTIMAL     (before buffer copy;
        //                                                       the image is freshly
        //                                                       created, so nothing
        //                                                       earlier accesses it)
        //     TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL (after copy, no mips)
        //     TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL     (before mip generation)
        //     TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL (after mip generation)
        //
        // The transitions that end in SHADER_READ_ONLY_OPTIMAL (the declared
        // layout of the bindless slots) make the image readable by all
        // Bindless_Reader_Pipeline_Stages (Shader_Stages.hpp). That includes
        // FRAGMENT_SHADER, so this function is recorded on a queue family
        // with graphics support: the graphics family, which every command
        // pool of the engine uses (Vulkan_Command_Pool).
        //
        // _command_buffer: must already be in the recording state
        // _mips: the mip levels affected by this transition
        void Transition_image_layout(
            VkCommandBuffer                _command_buffer,
            VkImage                        _image,
            VkImageLayout                  _old_layout,
            VkImageLayout                  _new_layout,
            const Vulkan_Barrier::Mip_Range& _mips
        );

        // One mip level to fill from a staging buffer: where its texels
        // start in the buffer, which level of the image receives them, and
        // the size of that level.
        struct Mip_Copy_Region
        {
            VkDeviceSize buffer_offset = 0;
            uint32_t     mip_level = 0;
            uint32_t     width = 0;
            uint32_t     height = 0;
        };

        // Copies the levels in _levels from a CPU-visible staging buffer
        // into an image that has already been transitioned to
        // TRANSFER_DST_OPTIMAL, in one vkCmdCopyBufferToImage. Each level's
        // texels are tightly packed (no row padding, as Image_Loader lays
        // them out) at its buffer_offset. The levels the image has beyond
        // those are left for Generate_mipmaps.
        //
        // _command_buffer: must already be in the recording state
        void Copy_buffer_to_image(
            VkCommandBuffer                _command_buffer,
            VkBuffer                       _buffer,
            VkImage                        _image,
            std::span<const Mip_Copy_Region> _levels
        );

        // Generates the mip levels _first_generated_level .. _mip_levels - 1
        // by repeatedly blitting each level into a half-size version of
        // itself (vkCmdBlitImage with linear filtering), starting from the
        // level just before _first_generated_level. Leaves EVERY mip level,
        // those that were copied from the staging buffer included, in
        // SHADER_READ_ONLY_OPTIMAL layout when done — no further transition
        // needed after calling this.
        //
        // Precondition: the image must already be in TRANSFER_DST_OPTIMAL
        // layout with levels 0 .. _first_generated_level - 1 filled (copied
        // from the staging buffer), and must have been created with
        // TRANSFER_SRC and TRANSFER_DST usage. The default of 1 generates
        // everything from level 0. _first_generated_level equal to
        // _mip_levels generates nothing and only brings the levels to
        // SHADER_READ_ONLY_OPTIMAL.
        //
        // Throws std::invalid_argument if _first_generated_level is 0 or
        // above _mip_levels. Throws std::runtime_error, before recording
        // anything, if _format lacks any of BLIT_SRC, BLIT_DST or
        // SAMPLED_IMAGE_FILTER_LINEAR with optimal tiling (checked only
        // when there is something to generate): the blits read and write
        // the same image, and a blit with VK_FILTER_LINEAR requires linear
        // filtering support on the source format.
        //
        // _command_buffer: must already be in the recording state
        void Generate_mipmaps(
            const Vulkan_Device& _device,
            VkCommandBuffer      _command_buffer,
            VkImage              _image,
            VkFormat             _format,
            int32_t              _width,
            int32_t              _height,
            uint32_t             _mip_levels,
            uint32_t             _first_generated_level = 1
        );

        // Computes how many mip levels a full mip chain needs for the given
        // dimensions: floor(log2(max(width, height))) + 1.
        // A 1024x1024 texture needs 11 levels (1024, 512, ..., 1).
        // Throws std::invalid_argument, in every build, if a dimension is
        // zero (a chain of no extent has no number of levels).
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