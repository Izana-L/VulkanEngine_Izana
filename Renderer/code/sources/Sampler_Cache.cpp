#include <Sampler_Cache.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <cassert>
#include <algorithm>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Sampler_Cache::Sampler_Cache(const Vulkan_Device& _device)
        : device_handle(_device.Get_logical_device_handle()),
        max_supported_anisotropy(1.0f)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Sampler_Cache");

        // Query the device's actual anisotropy limit so requested values
        // above it are clamped instead of causing a validation error.
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device.Get_physical_device_handle(), &properties);
        max_supported_anisotropy = properties.limits.maxSamplerAnisotropy;
    }

    // ---------- Destructor ----------
    Sampler_Cache::~Sampler_Cache()
    {
        // All cached samplers are destroyed together here — individual
        // samplers are shared by multiple textures, so they can't be
        // destroyed earlier without tracking reference counts, which
        // isn't needed since samplers are cheap and few in number.
        for (auto& [desc, sampler] : cache)
        {
            if (sampler != VK_NULL_HANDLE)
                vkDestroySampler(device_handle, sampler, nullptr);
        }
        cache.clear();
    }

    // ---------- Get_sampler ----------
    VkSampler Sampler_Cache::Get_sampler(const Sampler_Desc& _desc)
    {
        auto it = cache.find(_desc);
        if (it != cache.end())
            return it->second;

        VkSampler sampler = Create_sampler(_desc);
        cache.emplace(_desc, sampler);
        return sampler;
    }

    // ---------- Get_default_sampler ----------
    VkSampler Sampler_Cache::Get_default_sampler()
    {
        Sampler_Desc desc;
        desc.mag_filter = VK_FILTER_LINEAR;
        desc.min_filter = VK_FILTER_LINEAR;
        desc.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        desc.address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        desc.address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        desc.anisotropy = 16.0f;

        // 16 mip levels covers textures up to 32768px on a side —
        // far beyond MAX_TEXTURE_SIZE (8192) from MathConstants, so this
        // single default sampler is valid for every texture the engine
        // can load without per-texture max_lod tuning.
        desc.max_lod = 16.0f;

        return Get_sampler(desc);
    }

    // ---------- Create_sampler ----------
    VkSampler Sampler_Cache::Create_sampler(const Sampler_Desc& _desc) const
    {
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = _desc.mag_filter;
        sampler_info.minFilter = _desc.min_filter;
        sampler_info.mipmapMode = _desc.mipmap_mode;
        sampler_info.addressModeU = _desc.address_mode_u;
        sampler_info.addressModeV = _desc.address_mode_v;
        sampler_info.addressModeW = _desc.address_mode_u;   // match U for consistency

        // Clamp requested anisotropy to what the GPU actually supports.
        const float clamped_anisotropy =
            std::min(_desc.anisotropy, max_supported_anisotropy);

        sampler_info.anisotropyEnable = (clamped_anisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
        sampler_info.maxAnisotropy = clamped_anisotropy;

        // Border color only matters for CLAMP_TO_BORDER address mode,
        // which this engine doesn't currently use — left at a sane default.
        sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

        // Texel coordinates are normalized [0,1], not pixel coordinates —
        // standard for sampled textures (VK_TRUE would be for rare cases
        // like exact texel lookups, not used here).
        sampler_info.unnormalizedCoordinates = VK_FALSE;

        // Not a depth-comparison sampler (that's only for shadow maps,
        // which use a separate sampler configuration in a later phase).
        sampler_info.compareEnable = VK_FALSE;
        sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;

        sampler_info.minLod = 0.0f;
        sampler_info.maxLod = _desc.max_lod;
        sampler_info.mipLodBias = 0.0f;

        VkSampler sampler = VK_NULL_HANDLE;
        VkResult result = vkCreateSampler(device_handle, &sampler_info, nullptr, &sampler);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Sampler_Cache: failed to create sampler: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        return sampler;
    }

} // namespace Renderer