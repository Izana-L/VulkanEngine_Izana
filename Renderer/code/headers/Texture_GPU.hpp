#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Vulkan_Buffer_Utils.hpp>
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
    // Follows the same staging pattern as Mesh_GPU (Option B): the
    // constructor records commands into a VkCommandBuffer that is already
    // in the recording state — it does NOT submit or wait. Staging buffers
    // are kept alive as members until the caller submits, waits, and calls
    // Release_staging_buffers().
    //
    // Typical usage (mirrors Renderer::Upload_mesh):
    //   vkBeginCommandBuffer(transfer_cmd, ...);
    //   textures.emplace_back(device, allocator, transfer_cmd, image_data);
    //   vkEndCommandBuffer(transfer_cmd);
    //   vkQueueSubmit(...); vkQueueWaitIdle(...);
    //   textures.back().Release_staging_buffers();
    //
    // RAII: move-only, no copy. Destructor releases image, memory, and view.
    class Texture_GPU
    {
    public:

        // Records the full upload sequence into _transfer_cmd:
        //   1. Create staging buffer, copy _image_data.pixels into it
        //   2. Create the VkImage (sized for the full mip chain)
        //   3. Transition UNDEFINED -> TRANSFER_DST_OPTIMAL
        //   4. Copy staging buffer -> image (mip 0 only)
        //   5. Generate_mipmaps (blits + transitions, ends in SHADER_READ_ONLY)
        //   6. Create the VkImageView covering all mip levels
        //
        // _format: VK_FORMAT_R8G8B8A8_SRGB for color textures (albedo,
        //   emissive), VK_FORMAT_R8G8B8A8_UNORM for data textures (normal,
        //   metallic-roughness, AO). Caller decides based on texture role —
        //   ImageData itself doesn't know its color space.
        //
        // Throws std::invalid_argument if _format is block-compressed or
        // unsupported, or if _image_data.pixels does not hold exactly the
        // bytes its dimensions, mip levels and _format need. Throws
        // std::runtime_error if _format lacks, with optimal tiling, the
        // features every bindless texture needs
        // (Vulkan_Image_Utils::Bindless_Sampled_Format_Features) plus
        // TRANSFER_DST, and the blit features when a mip chain is
        // generated. On any exception every resource created so far is
        // released, and the commands already recorded into _transfer_cmd
        // must not be submitted.
        Texture_GPU(
            const Vulkan_Device& _device,
            VmaAllocator               _allocator,
            VkCommandBuffer            _transfer_cmd,
            const CoreTypes::ImageData& _image_data,
            VkFormat                   _format
        );

        ~Texture_GPU();

        Texture_GPU(const Texture_GPU&) = delete;
        Texture_GPU& operator=(const Texture_GPU&) = delete;

        Texture_GPU(Texture_GPU&& _other) noexcept;
        Texture_GPU& operator=(Texture_GPU&& _other) noexcept;

        // Destroys the staging buffer once the GPU has finished consuming it
        // (after the caller's vkQueueSubmit + vkQueueWaitIdle). Safe to call
        // even if already released — checks for VK_NULL_HANDLE internally.
        void Release_staging_buffers();

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

        // Validates the input and the format support, creates the staging
        // buffer, the image and its view, and records the upload. Called
        // once by the constructor, which releases everything on failure.
        void Record_upload(const Vulkan_Device& _device, VkCommandBuffer _transfer_cmd, const CoreTypes::ImageData& _image_data);

        void Destroy();

        // Still needed: image views are not memory, so they are created
        // and destroyed through the device, not through VMA.
        VkDevice     device_handle;
        VmaAllocator allocator;

        Vulkan_Image_Utils::Image_Allocation   image;
        VkImageView                            image_view;

        Vulkan_Buffer_Utils::Buffer_Allocation staging;

        uint32_t       width;
        uint32_t       height;
        uint32_t       mip_levels;
        VkFormat       format;
    };

} // namespace Renderer