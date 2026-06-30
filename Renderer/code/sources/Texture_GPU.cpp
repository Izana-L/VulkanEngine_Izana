#include <Texture_GPU.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <cassert>
#include <cstring>
#include <stdexcept>

namespace Renderer
{

    // ---------- Constructor ----------
    Texture_GPU::Texture_GPU(
        const Vulkan_Device& _device,
        VkCommandBuffer             _transfer_cmd,
        const CoreTypes::ImageData& _image_data,
        VkFormat                    _format)

        : device_handle(_device.Get_logical_device_handle()),
        image(VK_NULL_HANDLE),
        image_memory(VK_NULL_HANDLE),
        image_view(VK_NULL_HANDLE),
        staging_buffer(VK_NULL_HANDLE),
        staging_memory(VK_NULL_HANDLE),
        width(_image_data.width),
        height(_image_data.height),
        mip_levels(Vulkan_Image_Utils::Compute_mip_levels(_image_data.width, _image_data.height)),
        format(_format)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Texture_GPU");
        assert(_transfer_cmd != VK_NULL_HANDLE &&
            "Texture_GPU: transfer command buffer must be valid and already recording");
        assert(!_image_data.pixels.empty() &&
            "Texture_GPU: ImageData has no pixel data");
        assert(width > 0 && height > 0 &&
            "Texture_GPU: ImageData has zero dimensions");

        // =========================================================
        // Staging buffer
        // =========================================================

        // Assumes 4 bytes per pixel (RGBA8) — matches Image_Loader, which
        // always forces STBI_rgb_alpha regardless of the source file's
        // actual channel count.
        const VkDeviceSize image_size =
            static_cast<VkDeviceSize>(width) * height * 4;

        Vulkan_Buffer_Utils::Create_buffer(
            _device,
            image_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging_buffer,
            staging_memory
        );

        void* mapped = nullptr;
        vkMapMemory(device_handle, staging_memory, 0, image_size, 0, &mapped);
        std::memcpy(mapped, _image_data.pixels.data(), static_cast<size_t>(image_size));
        vkUnmapMemory(device_handle, staging_memory);

        // =========================================================
        // Image creation
        // =========================================================

        // TRANSFER_SRC_BIT is required even though this image is the upload
        // target, because Generate_mipmaps blits each mip level FROM the
        // previous level of this same image — the image is both source and
        // destination of those internal blits.
        Vulkan_Image_Utils::Create_image(
            _device,
            width, height,
            mip_levels,
            format,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            image,
            image_memory
        );

        // =========================================================
        // Upload + mipmap generation (recorded, not submitted here)
        // =========================================================

        // Transition the whole mip chain to TRANSFER_DST so mip 0 can be
        // copied into. Generate_mipmaps re-transitions levels individually
        // as it processes them.
        Vulkan_Image_Utils::Transition_image_layout(
            _transfer_cmd, image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            mip_levels
        );

        Vulkan_Image_Utils::Copy_buffer_to_image(
            _transfer_cmd, staging_buffer, image, width, height
        );

        if (mip_levels > 1)
        {
            // Generates mips 1..N from mip 0, leaves every level in
            // SHADER_READ_ONLY_OPTIMAL when done.
            Vulkan_Image_Utils::Generate_mipmaps(
                _device, _transfer_cmd, image, format,
                static_cast<int32_t>(width), static_cast<int32_t>(height),
                mip_levels
            );
        }
        else
        {
            // Single mip level — no blitting needed, just transition
            // straight to shader-readable.
            Vulkan_Image_Utils::Transition_image_layout(
                _transfer_cmd, image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                mip_levels
            );
        }

        // =========================================================
        // Image view
        // =========================================================

        image_view = Vulkan_Image_Utils::Create_image_view(
            _device, image, format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels
        );
    }

    // ---------- Destructor ----------
    Texture_GPU::~Texture_GPU()
    {
        Destroy();
    }

    // ---------- Destroy ----------
    void Texture_GPU::Destroy()
    {
        Release_staging_buffers();

        if (image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_handle, image_view, nullptr);
            image_view = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device_handle, image, nullptr);
            image = VK_NULL_HANDLE;
        }
        if (image_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_handle, image_memory, nullptr);
            image_memory = VK_NULL_HANDLE;
        }
    }

    // ---------- Release_staging_buffers ----------
    void Texture_GPU::Release_staging_buffers()
    {
        if (staging_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_handle, staging_buffer, nullptr);
            staging_buffer = VK_NULL_HANDLE;
        }
        if (staging_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_handle, staging_memory, nullptr);
            staging_memory = VK_NULL_HANDLE;
        }
    }

    // ---------- Move constructor ----------
    Texture_GPU::Texture_GPU(Texture_GPU&& _other) noexcept
        : device_handle(_other.device_handle),
        image(_other.image),
        image_memory(_other.image_memory),
        image_view(_other.image_view),
        staging_buffer(_other.staging_buffer),
        staging_memory(_other.staging_memory),
        width(_other.width),
        height(_other.height),
        mip_levels(_other.mip_levels),
        format(_other.format)
    {
        _other.image = VK_NULL_HANDLE;
        _other.image_memory = VK_NULL_HANDLE;
        _other.image_view = VK_NULL_HANDLE;
        _other.staging_buffer = VK_NULL_HANDLE;
        _other.staging_memory = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Texture_GPU& Texture_GPU::operator=(Texture_GPU&& _other) noexcept
    {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            image = _other.image;
            image_memory = _other.image_memory;
            image_view = _other.image_view;
            staging_buffer = _other.staging_buffer;
            staging_memory = _other.staging_memory;
            width = _other.width;
            height = _other.height;
            mip_levels = _other.mip_levels;
            format = _other.format;

            _other.image = VK_NULL_HANDLE;
            _other.image_memory = VK_NULL_HANDLE;
            _other.image_view = VK_NULL_HANDLE;
            _other.staging_buffer = VK_NULL_HANDLE;
            _other.staging_memory = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Getters ----------
    VkImageView Texture_GPU::Get_image_view() const
    {
        assert(image_view != VK_NULL_HANDLE &&
            "Get_image_view() called on a moved-from or destroyed Texture_GPU");
        return image_view;
    }

    VkImage Texture_GPU::Get_image() const
    {
        assert(image != VK_NULL_HANDLE &&
            "Get_image() called on a moved-from or destroyed Texture_GPU");
        return image;
    }

    uint32_t Texture_GPU::Get_width()      const { return width; }
    uint32_t Texture_GPU::Get_height()     const { return height; }
    uint32_t Texture_GPU::Get_mip_levels() const { return mip_levels; }
    VkFormat Texture_GPU::Get_format()     const { return format; }

} // namespace Renderer