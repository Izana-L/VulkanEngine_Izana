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
        anisotropy_enabled(_device.Is_sampler_anisotropy_enabled()),
        max_supported_anisotropy(_device.Get_max_sampler_anisotropy())
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Sampler_Cache");
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

    // ---------- Get_preset_desc ----------
    Sampler_Desc Sampler_Cache::Get_preset_desc(CoreTypes::Sampler_Preset _preset)
    {
        using CoreTypes::Sampler_Preset;

        // Anisotropy requested by the linear presets. Create_sampler clamps
        // it to maxSamplerAnisotropy and drops it without the feature.
        constexpr float LINEAR_ANISOTROPY = 16.0f;

        bool nearest = false;
        bool clamp = false;

        switch (_preset)
        {
        case Sampler_Preset::Linear_Repeat:  nearest = false; clamp = false; break;
        case Sampler_Preset::Linear_Clamp:   nearest = false; clamp = true;  break;
        case Sampler_Preset::Nearest_Repeat: nearest = true;  clamp = false; break;
        case Sampler_Preset::Nearest_Clamp:  nearest = true;  clamp = true;  break;
        default:
            throw std::invalid_argument("Sampler_Cache::Get_preset_desc: value is not a sampler preset");
        }

        const VkFilter             filter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        const VkSamplerAddressMode address = clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;

        Sampler_Desc desc;
        desc.mag_filter = filter;
        desc.min_filter = filter;
        desc.mipmap_mode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        desc.address_mode_u = address;
        desc.address_mode_v = address;

        // Anisotropic filtering blends several texels along the view
        // direction, which undoes the hard texel edges a nearest preset is
        // chosen for: 0 disables it.
        desc.anisotropy = nearest ? 0.0f : LINEAR_ANISOTROPY;

        // The image view bounds the mip chain of each texture (see
        // Sampler_Desc::max_lod).
        desc.max_lod = VK_LOD_CLAMP_NONE;

        return desc;
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

        // Anisotropy is used only when the device FEATURE was enabled
        // (Vulkan_Device enables it when supported) and the requested level
        // is above 1, clamped to the device limit. Without the feature the
        // sampler is created isotropic, which is valid on every device.
        const float clamped_anisotropy =
            std::min(_desc.anisotropy, max_supported_anisotropy);

        const bool use_anisotropy = anisotropy_enabled && clamped_anisotropy > 1.0f;

        sampler_info.anisotropyEnable = use_anisotropy ? VK_TRUE : VK_FALSE;
        sampler_info.maxAnisotropy = use_anisotropy ? clamped_anisotropy : 1.0f;

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
        VK_CHECK(vkCreateSampler(device_handle, &sampler_info, nullptr, &sampler),
            "Sampler_Cache: failed to create sampler");

        return sampler;
    }

} // namespace Renderer