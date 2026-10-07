#include <Vulkan_Image_Utils.hpp>
#include <Shader_Stages.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <string>
#include <cassert>
#include <algorithm>
#include <bit>
#include <vector>

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
            VkImageUsageFlags _usage,
            Image_Memory      _memory)
        {
            // Enforced in every build, before anything is created: the
            // size, the format and the usage are checked against what the
            // device can create instead of being handed to the driver.
            if (_allocator == VK_NULL_HANDLE)
                throw std::invalid_argument("Create_image: null allocator");

            Require_image_support(_allocator, _format, _tiling, _usage, _width, _height, _mip_levels, "Create_image");

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

            // Always UNDEFINED at creation. The specification only allows
            // UNDEFINED or PREINITIALIZED here, and PREINITIALIZED only
            // matters for LINEAR images filled by the CPU. The first real
            // layout is set by the first user of the image:
            //   uploaded textures - UNDEFINED -> TRANSFER_DST_OPTIMAL before
            //                       the buffer copy (Transition_image_layout);
            //   storage images    - UNDEFINED -> TRANSFER_DST_OPTIMAL for the
            //                       initial clear, then UNDEFINED -> GENERAL
            //                       before every compute write (Storage_Image,
            //                       through Record_image_barrier);
            //   attachments       - the render pass, through the attachment
            //                       initialLayout and its subpass layout.
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
            Image_Allocation out{};

            if (_memory == Image_Memory::Lazy_Or_Dedicated)
            {
                // Lazily allocated memory first: on tile-based GPUs a
                // transient attachment then never gets backing memory at
                // all.
                VmaAllocationCreateInfo lazy_info{};
                lazy_info.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;

                const VkResult lazy_result = vmaCreateImage(_allocator, &image_info, &lazy_info,
                                                            &out.image, &out.allocation, nullptr);

                if (lazy_result == VK_SUCCESS)
                {
                    out.lazily_allocated = true;
                    return out;
                }

                // The output of the failed call is not used.
                out = Image_Allocation{};

                // Only "this device has no lazily allocated memory type"
                // (desktop GPUs) falls through to the dedicated allocation
                // below. Out of memory or a lost device is reported as
                // such: retrying it as if the memory type did not exist
                // would hide the first error behind whatever the second
                // attempt says.
                if (lazy_result != VK_ERROR_FEATURE_NOT_PRESENT)
                    VK_CHECK(lazy_result, "Failed to create image (lazily allocated memory)");
            }

            VmaAllocationCreateInfo alloc_info{};
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

            if (_memory != Image_Memory::Device_Local)
                alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

            // Creates the image, queries its requirements, allocates and
            // binds. Nothing leaks if any of those steps fails.
            VK_CHECK(vmaCreateImage(
                _allocator,
                &image_info,
                &alloc_info,
                &out.image,
                &out.allocation,
                nullptr
            ), "Failed to create image");

            return out;
        }

        // ---------- Destroy_image ----------
        void Destroy_image(VmaAllocator _allocator, Image_Allocation& _image)
        {
            if (_image.image != VK_NULL_HANDLE) {
                vmaDestroyImage(_allocator, _image.image, _image.allocation);
                _image = Image_Allocation{};
            }
        }

        // ---------- Require_image_support ----------
        void Require_image_support(
            VmaAllocator      _allocator,
            VkFormat          _format,
            VkImageTiling     _tiling,
            VkImageUsageFlags _usage,
            uint32_t          _width,
            uint32_t          _height,
            uint32_t          _mip_levels,
            const char*       _caller)
        {
            // The text of the messages is only built when one is thrown: the
            // check runs for every image that is created.
            const auto description = [&]()
            {
                return std::string(_caller) + ": a " + std::to_string(_width) + "x" + std::to_string(_height) +
                       " image of format " + Vulkan_Utils::Vk_format_to_string(_format);
            };

            if (_width == 0 || _height == 0)
                throw std::invalid_argument(description() + " has a dimension of 0");

            if (_mip_levels == 0)
                throw std::invalid_argument(description() + " needs at least one mip level");

            VmaAllocatorInfo allocator_info{};
            vmaGetAllocatorInfo(_allocator, &allocator_info);

            // The limits of image creation (imageCreateMaxExtent,
            // imageCreateMaxMipLevels of the specification) depend on the
            // format, tiling and usage, not only on maxImageDimension2D, so
            // the device is asked about this exact combination.
            VkImageFormatProperties properties{};
            const VkResult result = vkGetPhysicalDeviceImageFormatProperties(allocator_info.physicalDevice, _format,
                VK_IMAGE_TYPE_2D, _tiling, _usage, 0, &properties);

            if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
                throw std::runtime_error(description() + " cannot be created on this device with this tiling and usage");

            VK_CHECK(result, (std::string(_caller) + ": query the image format limits").c_str());

            if (_width > properties.maxExtent.width || _height > properties.maxExtent.height)
            {
                throw std::runtime_error(description() + " is larger than this device allows (at most " +
                                         std::to_string(properties.maxExtent.width) + "x" + std::to_string(properties.maxExtent.height) +
                                         " for this format, tiling and usage)");
            }

            if (_mip_levels > properties.maxMipLevels)
            {
                throw std::runtime_error(description() + " with " + std::to_string(_mip_levels) +
                                         " mip levels has more than this device allows (at most " +
                                         std::to_string(properties.maxMipLevels) + ")");
            }
        }

        // ---------- Require_optimal_tiling_features ----------
        void Require_optimal_tiling_features(
            const Vulkan_Device& _device,
            VkFormat             _format,
            VkFormatFeatureFlags _required,
            const char*          _caller)
        {
            VkFormatProperties format_properties{};
            vkGetPhysicalDeviceFormatProperties(_device.Get_physical_device_handle(), _format, &format_properties);

            const VkFormatFeatureFlags missing = _required & ~format_properties.optimalTilingFeatures;

            if (missing == 0)
                return;

            // Names of the features the engine requires somewhere; any other
            // bit is reported by its value.
            struct Feature_Name
            {
                VkFormatFeatureFlagBits bit;
                const char*             name;
            };

            static constexpr Feature_Name feature_names[] =
            {
                { VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,               "SAMPLED_IMAGE" },
                { VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "SAMPLED_IMAGE_FILTER_LINEAR" },
                { VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,               "STORAGE_IMAGE" },
                { VK_FORMAT_FEATURE_BLIT_SRC_BIT,                    "BLIT_SRC" },
                { VK_FORMAT_FEATURE_BLIT_DST_BIT,                    "BLIT_DST" },
                { VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,                "TRANSFER_SRC" },
                { VK_FORMAT_FEATURE_TRANSFER_DST_BIT,                "TRANSFER_DST" },
                { VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,            "COLOR_ATTACHMENT" },
                { VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,      "COLOR_ATTACHMENT_BLEND" },
            };

            std::string missing_names;
            VkFormatFeatureFlags unnamed = missing;

            for (const Feature_Name& feature : feature_names)
            {
                if ((missing & feature.bit) == 0)
                    continue;

                missing_names += missing_names.empty() ? "" : ", ";
                missing_names += feature.name;
                unnamed &= ~static_cast<VkFormatFeatureFlags>(feature.bit);
            }

            if (unnamed != 0)
            {
                missing_names += missing_names.empty() ? "" : ", ";
                missing_names += "other flags (value " + std::to_string(unnamed) + ")";
            }

            throw std::runtime_error(std::string(_caller) + ": format " + std::to_string(static_cast<int>(_format)) +
                                     " lacks " + missing_names + " with optimal tiling on this device");
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
            VK_CHECK(vkCreateImageView(_device, &view_info, nullptr, &image_view),
                "Failed to create image view");

            return image_view;
        }

        // ---------- Transition_image_layout ----------
        void Transition_image_layout(
            VkCommandBuffer                _command_buffer,
            VkImage                        _image,
            VkImageLayout                  _old_layout,
            VkImageLayout                  _new_layout,
            const Vulkan_Barrier::Mip_Range& _mips)
        {
            assert(_command_buffer != VK_NULL_HANDLE &&
                "Transition_image_layout() called with a null command buffer");
            assert(_image != VK_NULL_HANDLE &&
                "Transition_image_layout() called with a null image");

            // Each supported transition has a specific pair of access masks
            // and pipeline stages — these tell the GPU what kind of work
            // must finish before the transition and what kind of work must
            // wait until after it, so the barrier actually synchronizes
            // correctly instead of just changing the layout label.
            Vulkan_Barrier::Access_Scope source;
            Vulkan_Barrier::Access_Scope destination;

            if (_old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
                _new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            {
                // Nothing to wait on: the image was just created and no
                // earlier command accesses it. Transfer writes must happen
                // after this barrier. An image that was already in use needs
                // its earlier accesses in the source scope, so it goes
                // through Vulkan_Barrier::Record_image_barrier instead.
                source = { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0 };
                destination = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
            }
            else if (_old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                _new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                // Transfer writes must finish before shader reads begin, in
                // every stage that can read the image through the bindless
                // set (Bindless_Reader_Pipeline_Stages, Shader_Stages.hpp).
                source = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
                destination = { Bindless_Reader_Pipeline_Stages, VK_ACCESS_SHADER_READ_BIT };
            }
            else if (_old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                _new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            {
                // Used between mip levels during Generate_mipmaps: the level
                // just written becomes the source for blitting into the next.
                source = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
                destination = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT };
            }
            else if (_old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                _new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                // Used after Generate_mipmaps finishes: the last mip level
                // (still TRANSFER_SRC from being blit source) becomes readable
                // by every bindless reader stage.
                source = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT };
                destination = { Bindless_Reader_Pipeline_Stages, VK_ACCESS_SHADER_READ_BIT };
            }
            else
            {
                // Deliberately no branch for GENERAL: the layouts do not say
                // which stages write or read the image, and a guess would
                // record wrong stages and accesses without any error.
                throw std::runtime_error("Transition_image_layout: unsupported layout transition; only the texture upload "
                                         "transitions are derived from the layouts. Record any other transition with "
                                         "Vulkan_Barrier::Record_image_barrier and explicit source and destination scopes.");
            }

            Vulkan_Barrier::Record_image_barrier(_command_buffer, _image, _old_layout, _new_layout, source, destination, _mips);
        }

        // ---------- Copy_buffer_to_image ----------
        void Copy_buffer_to_image(
            VkCommandBuffer                  _command_buffer,
            VkBuffer                         _buffer,
            VkImage                          _image,
            std::span<const Mip_Copy_Region> _levels)
        {
            assert(_command_buffer != VK_NULL_HANDLE && "Copy_buffer_to_image() called with a null command buffer");
            assert(_buffer != VK_NULL_HANDLE && "Copy_buffer_to_image() called with a null buffer");
            assert(_image != VK_NULL_HANDLE && "Copy_buffer_to_image() called with a null image");

            if (_levels.empty())
                throw std::invalid_argument("Copy_buffer_to_image: no mip level to copy");

            std::vector<VkBufferImageCopy> regions;
            regions.reserve(_levels.size());

            for (const Mip_Copy_Region& level : _levels)
            {
                VkBufferImageCopy region{};
                region.bufferOffset = level.buffer_offset;

                // 0 means "tightly packed" — the buffer has no row padding,
                // matches how Image_Loader lays out pixel data.
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;

                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = level.mip_level;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;

                region.imageOffset = { 0, 0, 0 };
                region.imageExtent = { level.width, level.height, 1 };

                regions.push_back(region);
            }

            vkCmdCopyBufferToImage(
                _command_buffer,
                _buffer,
                _image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                static_cast<uint32_t>(regions.size()), regions.data()
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
            uint32_t             _mip_levels,
            uint32_t             _first_generated_level)
        {
            assert(_command_buffer != VK_NULL_HANDLE &&
                "Generate_mipmaps() called with a null command buffer");
            assert(_image != VK_NULL_HANDLE &&
                "Generate_mipmaps() called with a null image");

            if (_mip_levels == 0 || _first_generated_level == 0 || _first_generated_level > _mip_levels)
                throw std::invalid_argument("Generate_mipmaps: the first generated level (" + std::to_string(_first_generated_level) +
                                            ") must be between 1 and the number of levels (" + std::to_string(_mip_levels) + ")");

            // Checked in every build, before anything is recorded: each level
            // is blitted from the previous one of the same image, so the
            // format must be both a blit source and a blit destination, and
            // VK_FILTER_LINEAR requires linear filtering on the source
            // format. The 8-bit color formats support all three on every
            // device; R32_SFLOAT, for example, is not guaranteed linear
            // filtering. Only needed when there is something to blit.
            if (_first_generated_level < _mip_levels)
            {
                Require_optimal_tiling_features(_device, _format,
                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                    "Generate_mipmaps");
            }

            // The levels below the last copied one were filled from the
            // staging buffer and are never a blit source: straight to
            // shader-readable. The last copied level is the first blit
            // source and is handled by the loop below.
            if (_first_generated_level > 1)
            {
                Transition_image_layout(_command_buffer, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    { 0, _first_generated_level - 1 });
            }

            // Size of the level the first blit reads.
            int32_t mip_width = std::max(_width >> (_first_generated_level - 1), 1);
            int32_t mip_height = std::max(_height >> (_first_generated_level - 1), 1);

            // For each level i, blit from level i-1 (the previous, already
            // filled level) into level i at half the size. Level
            // _first_generated_level - 1 is assumed to be in
            // TRANSFER_DST_OPTIMAL (just copied from the staging buffer by
            // the caller).
            for (uint32_t i = _first_generated_level; i < _mip_levels; ++i)
            {
                // Level i-1: TRANSFER_DST -> TRANSFER_SRC. It was either just
                // copied into or just blitted into (mip i-1 from a previous
                // loop iteration) — either way it's currently TRANSFER_DST
                // and needs to become the blit source.
                Transition_image_layout(_command_buffer, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    { i - 1, 1 });

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

                // Level i-1: TRANSFER_SRC -> SHADER_READ_ONLY. This level is
                // done — it was only needed as a blit source. The
                // destination covers every stage that can read the image
                // through the bindless set (Bindless_Reader_Pipeline_Stages,
                // Shader_Stages.hpp).
                Transition_image_layout(_command_buffer, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    { i - 1, 1 });

                mip_width = next_width;
                mip_height = next_height;
            }

            // The last mip level was only ever a copy or blit DESTINATION,
            // never a source — the loop above only transitioned the levels
            // before it. Transition it separately: TRANSFER_DST ->
            // SHADER_READ_ONLY, with the same destination stages as the
            // per-level barrier above.
            Transition_image_layout(_command_buffer, _image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                { _mip_levels - 1, 1 });
        }

        // ---------- To_vk_format ----------
        VkFormat To_vk_format(CoreTypes::Pixel_Format _format)
        {
            switch (_format)
            {
            case CoreTypes::Pixel_Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
            case CoreTypes::Pixel_Format::RGBA8_SRGB:  return VK_FORMAT_R8G8B8A8_SRGB;
            case CoreTypes::Pixel_Format::BC7_SRGB:    return VK_FORMAT_BC7_SRGB_BLOCK;
            case CoreTypes::Pixel_Format::BC5_UNORM:   return VK_FORMAT_BC5_UNORM_BLOCK;
            case CoreTypes::Pixel_Format::R8_UNORM:    return VK_FORMAT_R8_UNORM;
            case CoreTypes::Pixel_Format::RG8_UNORM:   return VK_FORMAT_R8G8_UNORM;
            case CoreTypes::Pixel_Format::R32_SFLOAT:  return VK_FORMAT_R32_SFLOAT;
            }

            throw std::invalid_argument("To_vk_format: unknown Pixel_Format " +
                std::to_string(static_cast<unsigned>(_format)));
        }

        // ---------- Bytes_per_pixel ----------
        uint32_t Bytes_per_pixel(VkFormat _format)
        {
            switch (_format)
            {
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R8_SRGB:           return 1;
            case VK_FORMAT_R8G8_UNORM:        return 2;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_R32_SFLOAT:        return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
            default:                          return 0;   // block-compressed or unsupported
            }
        }

        // ---------- Compute_mip_levels ----------
        uint32_t Compute_mip_levels(uint32_t _width, uint32_t _height)
        {
            // Enforced in every build: the logarithm of 0 is not a number of
            // levels, and converting it would be undefined behavior.
            if (_width == 0 || _height == 0)
                throw std::invalid_argument("Compute_mip_levels: a dimension is zero (" +
                                            std::to_string(_width) + "x" + std::to_string(_height) + ")");

            // bit_width(n) is floor(log2(n)) + 1 for n > 0, computed on the
            // integer itself instead of through floating point.
            return static_cast<uint32_t>(std::bit_width(std::max(_width, _height)));
        }

    } // namespace Vulkan_Image_Utils
} // namespace Renderer