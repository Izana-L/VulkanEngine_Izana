#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Handles.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Upload_Context.hpp>
#include <ImageData.hpp>

#include <cstdint>

namespace Renderer_System
{

    // Texture_GPU: owns the GPU-side resources for a single texture —
    // VkImage, its backing memory, and a VkImageView with a full mip chain.
    //
    // Does NOT own a VkSampler, and is not tied to one. Samplers are
    // shared across textures via Sampler_Cache and live in their own
    // bindless array (set 3, binding 1): the texture is registered alone
    // (set 3, binding 0) and the shader pairs it, at the point of use,
    // with whichever sampler preset the material selects.
    //
    // Follows the same staging pattern as the mesh uploads: the constructor
    // records commands into a VkCommandBuffer that is already in the
    // recording state — it does NOT submit or wait. The staging memory is
    // not the texture's: it comes from the Upload_Context of the transfer
    // (Upload_Context::Create_staging), which owns it together with the
    // staging buffers of every other asset of the batch and frees them all
    // at End() (after the submit has completed) or Abort(). The texture
    // therefore has nothing to release afterwards.
    //
    // Typical usage (mirrors Renderer::Upload_batch):
    //   upload.Run([&](VkCommandBuffer cmd)
    //   {
    //       textures.emplace_back(device, allocator, upload, cmd, image_data, format);
    //   });
    // Run submits and waits, then frees the staging buffers (End), and
    // undoes the transfer (Abort) when anything throws in between.
    //
    // RAII: move-only, no copy. The image and its view are owned by
    // Unique_Image / Unique_Image_View: destruction and moves are theirs.
    class Texture_GPU
    {
    public:

        // Records the full upload sequence into _transfer_cmd:
        //   1. Take a staging buffer from _upload and copy
        //      _image_data.pixels into it
        //   2. Create the VkImage (sized for the full mip chain)
        //   3. Transition UNDEFINED -> TRANSFER_DST_OPTIMAL
        //   4. Copy staging buffer -> image, one region for every mip level
        //      that _image_data carries
        //   5. Generate_mipmaps for the levels it does not carry (blits +
        //      transitions, ends in SHADER_READ_ONLY); when it carries the
        //      whole chain only the transition to SHADER_READ_ONLY remains
        //   6. Create the VkImageView covering all mip levels
        //
        // _format: VK_FORMAT_R8G8B8A8_SRGB for color textures (albedo,
        //   emissive), VK_FORMAT_R8G8B8A8_UNORM for data textures (normal,
        //   metallic-roughness, AO). Caller decides based on texture role —
        //   ImageData itself doesn't know its color space.
        //
        // _image_data.mip_levels: how many levels of the chain the pixels
        //   hold (ImageData.hpp: stored consecutively, largest first). 1 is
        //   the base level only, and every other level is generated on the
        //   GPU; a pre-built chain is uploaded as it is and only the missing
        //   levels, if any, are generated from the last one it carries.
        //
        // _allocator and _upload must belong to the same device as _device.
        //
        // Throws std::invalid_argument if a dimension of _image_data is
        // zero, if _format is block-compressed or unsupported, if
        // _image_data.mip_levels is 0 or beyond the chain its dimensions
        // have, or if _image_data.pixels does not hold exactly the bytes its
        // dimensions, mip levels and _format need. Throws
        // std::runtime_error if the device cannot create an image of that
        // size and format (Vulkan_Image_Utils::Require_image_support) or if
        // _format lacks, with optimal tiling, the features every bindless
        // texture needs (Vulkan_Image_Utils::Bindless_Sampled_Format_Features)
        // plus TRANSFER_DST, and the blit features when levels are
        // generated. On any exception the image and the view are released;
        // the staging buffers already taken from _upload are released by its
        // Abort(), and the commands already recorded into _transfer_cmd must
        // not be submitted.
        Texture_GPU(
            const Vulkan_Device&        _device,
            VmaAllocator                _allocator,
            Upload_Context&             _upload,
            VkCommandBuffer             _transfer_cmd,
            const CoreTypes::ImageData& _image_data,
            VkFormat                    _format
        );

        ~Texture_GPU() = default;

        Texture_GPU(const Texture_GPU&) = delete;
        Texture_GPU& operator=(const Texture_GPU&) = delete;

        Texture_GPU(Texture_GPU&& _other) noexcept = default;
        Texture_GPU& operator=(Texture_GPU&& _other) noexcept = default;

        // =========================================================
        // Getters
        // =========================================================

        VkImageView Get_image_view()  const;
        VkImage     Get_image()       const;
        uint32_t    Get_width()       const;
        uint32_t    Get_height()      const;
        uint32_t    Get_mip_levels()  const;
        VkFormat    Get_format()      const;

    private:

        // Validates the input and the format support, takes the staging
        // buffer, creates the image and its view, and records the upload.
        // Called once by the constructor; whatever it created before a
        // failure is released by the members.
        void Record_upload(const Vulkan_Device& _device, Upload_Context& _upload, VkCommandBuffer _transfer_cmd,
                           const CoreTypes::ImageData& _image_data);

        // Still needed: image views are not memory, so they are created
        // through the device, not through VMA.
        VkDevice     device_handle;
        VmaAllocator allocator;

        // The view is declared after the image, so the destructor releases
        // it first. A move assignment assigns the members in declaration
        // order and releases the old image before the old view; Vulkan only
        // requires that no pending work uses either of them, not an order
        // between an image and its views.
        Unique_Image      image;
        Unique_Image_View image_view;

        uint32_t       width;
        uint32_t       height;
        uint32_t       mip_levels;
        VkFormat       format;
    };

} // namespace Renderer
