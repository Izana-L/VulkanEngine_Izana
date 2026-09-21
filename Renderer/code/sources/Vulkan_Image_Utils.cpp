#include <Vulkan_Image_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <cassert>
#include <algorithm>
#include <cmath>

namespace Renderer_System
{
    namespace Vulkan_Image_Utils
    {

        // ---------- Create_image ----------
        Image_Allocation Create_image(
            VmaAllocator      _allocator,
            uint32_t          _width,
            uint32_t          _height,
            uint32_t          _mip_levels,
            VkFormat          _format,
            VkImageTiling     _tiling,
            VkImageUsageFlags _usage)
        {
            assert(_allocator != VK_NULL_HANDLE && "Create_image() called with a null allocator");
            assert(_width > 0 && _height > 0 && "Create_image() called with zero dimensions");
            assert(_mip_levels > 0 && "Create_image() called with zero mip levels");

            // ---------- Image creation ----------
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.extent.width = _width;
            image_info.extent.height = _height;
            image_info.extent.depth = 1;
            image_info.mipLevels = _mip_levels;
            image_info.arrayLayers = 1;
            image_info.format = _format;
            image_info.tiling = _tiling;

            // Always UNDEFINED at creation — the first transition (in
            // Transition_image_layout) moves it to TRANSFER_DST_OPTIMAL
            // before any data is copied in.
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            image_info.usage = _usage;

            // EXCLUSIVE: same reasoning as Vulkan_Buffer_Utils::Create_buffer —
            // only the graphics queue touches this image, no need for the
            // more expensive CONCURRENT sharing mode.
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            // No multisampling for regular textures (that's for render
            // target attachments, not sampled textures).
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;

            // ---------- Memory allocation ----------
            VmaAllocationCreateInfo alloc_info{};
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

            // Textures and depth buffers are large and long-lived, so each
            // gets its own VkDeviceMemory instead of a slice of a shared
            // block. Do NOT copy this flag onto small, numerous allocations.
            alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

            Image_Allocation out{};

            // Creates the image, queries its requirements, allocates and
            // binds. Nothing leaks if any of those steps fails.
            VkResult result = vmaCreateImage(
                _allocator,
                &image_info,
                &alloc_info,
                &out.image,
                &out.allocation,
                nullptr
            );

            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create image: " + Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            return out;
        }

        // ---------- Destroy_image ----------
        void Destroy_image(VmaAllocator _allocator, Image_Allocation& _image)
        {
            if (_image.image != VK_NULL_HANDLE) {
                vmaDestroyImage(_allocator, _image.image, _image.allocation);
                _image.image = VK_NULL_HANDLE;
                _image.allocation = VK_NULL_HANDLE;
            }
        }

        // ---------- Create_image_view ----------
        VkImageView Create_image_view(
            VkDevice              _device,
            VkImage               _image,
            VkFormat              _format,
            VkImageAspectFlags    _aspect_flags,
            uint32_t              _mip_levels)
        {
            assert(_image != VK_NULL_HANDLE && "Create_image_view() called with a null image");
            assert(_mip_levels > 0 && "Create_image_view() called with zero mip levels");

            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = _image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = _format;

            view_info.subresourceRange.aspectMask = _aspect_flags;
            view_info.subresourceRange.baseMipLevel = 0;
            view_info.subresourceRange.levelCount = _mip_levels;
            view_info.subresourceRange.baseArrayLayer = 0;
            view_info.subresourceRange.layerCount = 1;

            VkImageView image_view = VK_NULL_HANDLE;
            VkResult result = vkCreateImageView(
                _device, &view_info, nullptr, &image_view);

            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create image view: " + Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            return image_view;
        }

        // ---------- Transition_image_layout ----------
        void Transition_image_layout(
            VkCommandBuffer _command_buffer,
            VkImage         _image,
            VkImageLayout   _old_layout,
            VkImageLayout   _new_layout,
            uint32_t        _mip_levels)
        {
            assert(_command_buffer != VK_NULL_HANDLE &&
                "Transition_image_layout() called with a null command buffer");
            assert(_image != VK_NULL_HANDLE &&
                "Transition_image_layout() called with a null image");

            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = _old_layout;
            barrier.newLayout = _new_layout;

            // Not transferring queue family ownership — same queue does
            // everything (graphics queue handles transfers too here).
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            barrier.image = _image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = _mip_levels;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;

            VkPipelineStageFlags source_stage;
            VkPipelineStageFlags destination_stage;

            // Each supported transition has a specific pair of access masks
            // and pipeline stages — these tell the GPU what kind of work
            // must finish before the transition and what kind of work must
            // wait until after it, so the barrier actually synchronizes
            // correctly instead of just changing the layout label.

            if (_old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
                _new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            {
                // Nothing to wait on (image is fresh) — transfer writes
                // must happen after this barrier.
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

                source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
            else if (_old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                _new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                // Transfer writes must finish before shader reads begin.
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            }
            else if (_old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                _new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            {
                // Used between mip levels during Generate_mipmaps: the level
                // just written becomes the source for blitting into the next.
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

                source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
            else if (_old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                _new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                // Used after Generate_mipmaps finishes: the last mip level
                // (still TRANSFER_SRC from being blit source) becomes readable.
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            }
            else
            {
                throw std::runtime_error(
                    "Transition_image_layout: unsupported layout transition. "
                    "Add a new branch here if this transition is genuinely needed."
                );
            }

            vkCmdPipelineBarrier(
                _command_buffer,
                source_stage, destination_stage,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier
            );
        }

        // ---------- Copy_buffer_to_image ----------
        void Copy_buffer_to_image(
            VkCommandBuffer _command_buffer,
            VkBuffer        _buffer,
            VkImage         _image,
            uint32_t        _width,
            uint32_t        _height)
        {
            assert(_command_buffer != VK_NULL_HANDLE &&
                "Copy_buffer_to_image() called with a null command buffer");
            assert(_buffer != VK_NULL_HANDLE &&
                "Copy_buffer_to_image() called with a null buffer");
            assert(_image != VK_NULL_HANDLE &&
                "Copy_buffer_to_image() called with a null image");

            VkBufferImageCopy region{};
            region.bufferOffset = 0;

            // 0 means "tightly packed" — the buffer has no row padding,
            // matches how Image_Loader lays out pixel data.
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;

            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;   // mip 0 only — mips are generated separately
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;

            region.imageOffset = { 0, 0, 0 };
            region.imageExtent = { _width, _height, 1 };

            vkCmdCopyBufferToImage(
                _command_buffer,
                _buffer,
                _image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &region
            );
        }

        // ---------- Generate_mipmaps ----------
        void Generate_mipmaps(
            const Vulkan_Device& _device,
            VkCommandBuffer      _command_buffer,
            VkImage              _image,
            VkFormat             _format,
            int32_t              _width,
            int32_t              _height,
            uint32_t             _mip_levels)
        {
            assert(_command_buffer != VK_NULL_HANDLE &&
                "Generate_mipmaps() called with a null command buffer");
            assert(_image != VK_NULL_HANDLE &&
                "Generate_mipmaps() called with a null image");
            assert(_mip_levels > 0 &&
                "Generate_mipmaps() called with zero mip levels");

            // Verify the format supports linear filtering for blit — required
            // for vkCmdBlitImage with VK_FILTER_LINEAR. Almost all standard
            // 8-bit formats support this; this assert catches the rare format
            // that doesn't before producing visually broken mips.
            VkFormatProperties format_properties{};
            vkGetPhysicalDeviceFormatProperties(
                _device.Get_physical_device_handle(), _format, &format_properties);

            assert((format_properties.optimalTilingFeatures &
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
                "Generate_mipmaps: image format does not support linear blitting");

            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.image = _image;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.subresourceRange.levelCount = 1;

            int32_t mip_width = _width;
            int32_t mip_height = _height;

            // For each level i (starting at 1), blit from level i-1 (the
            // previous, already-filled level) into level i at half the size.
            // Mip 0 is assumed to already be in TRANSFER_DST_OPTIMAL (just
            // copied from the staging buffer by the caller).
            for (uint32_t i = 1; i < _mip_levels; ++i)
            {
                // Transition level i-1: TRANSFER_DST -> TRANSFER_SRC.
                // It was either just copied into (mip 0) or just blitted into
                // (mip i-1 from a previous loop iteration) — either way it's
                // currently TRANSFER_DST and needs to become the blit source.
                barrier.subresourceRange.baseMipLevel = i - 1;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

                vkCmdPipelineBarrier(
                    _command_buffer,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0,
                    0, nullptr,
                    0, nullptr,
                    1, &barrier
                );

                // Blit level i-1 -> level i, halving dimensions (clamped to 1).
                const int32_t next_width = mip_width > 1 ? mip_width / 2 : 1;
                const int32_t next_height = mip_height > 1 ? mip_height / 2 : 1;

                VkImageBlit blit{};
                blit.srcOffsets[0] = { 0, 0, 0 };
                blit.srcOffsets[1] = { mip_width, mip_height, 1 };
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = i - 1;
                blit.srcSubresource.baseArrayLayer = 0;
                blit.srcSubresource.layerCount = 1;

                blit.dstOffsets[0] = { 0, 0, 0 };
                blit.dstOffsets[1] = { next_width, next_height, 1 };
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = i;
                blit.dstSubresource.baseArrayLayer = 0;
                blit.dstSubresource.layerCount = 1;

                vkCmdBlitImage(
                    _command_buffer,
                    _image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    _image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit,
                    VK_FILTER_LINEAR
                );

                // Transition level i-1: TRANSFER_SRC -> SHADER_READ_ONLY.
                // This level is done — it was only needed as a blit source.
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                vkCmdPipelineBarrier(
                    _command_buffer,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0,
                    0, nullptr,
                    0, nullptr,
                    1, &barrier
                );

                mip_width = next_width;
                mip_height = next_height;
            }

            // The last mip level was only ever a blit DESTINATION, never a
            // source — its loop iteration above only transitioned levels
            // 0..mip_levels-2. Transition the final level separately:
            // TRANSFER_DST -> SHADER_READ_ONLY.
            barrier.subresourceRange.baseMipLevel = _mip_levels - 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(
                _command_buffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier
            );
        }

        // ---------- Compute_mip_levels ----------
        uint32_t Compute_mip_levels(uint32_t _width, uint32_t _height)
        {
            assert(_width > 0 && _height > 0 &&
                "Compute_mip_levels() called with zero dimensions");

            const uint32_t max_dimension = std::max(_width, _height);

            return static_cast<uint32_t>(
                std::floor(std::log2(static_cast<double>(max_dimension)))) + 1;
        }

    } // namespace Vulkan_Image_Utils
} // namespace Renderer