#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Hash.hpp>
#include <Sampler_Preset.hpp>

#include <cstdint>
#include <unordered_map>

namespace Renderer_System
{

    // Sampler_Desc: the subset of VkSamplerCreateInfo parameters that
    // actually vary across textures in this engine. Used as a hashable,
    // comparable key into Sampler_Cache.
    //
    // Kept deliberately small — only fields that actually differ between
    // the sampler presets (see Sampler_Cache::Get_preset_desc). If a new
    // sampling need arises (e.g. VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
    // for a specific effect), add the field here and a preset for it,
    // rather than creating samplers ad-hoc elsewhere.
    struct Sampler_Desc
    {
        VkFilter             mag_filter = VK_FILTER_LINEAR;
        VkFilter             min_filter = VK_FILTER_LINEAR;
        VkSamplerMipmapMode  mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VkSamplerAddressMode address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerAddressMode address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        // Anisotropic filtering level. 0 = disabled. Typical values: 8 or 16.
        // Clamped to the device's maxSamplerAnisotropy limit when the
        // sampler is actually created.
        float anisotropy = 16.0f;

        // Highest mip level this sampler may select (maxLod).
        // VK_LOD_CLAMP_NONE leaves the limit to the image view, which
        // already exposes exactly the mip levels each texture has: one
        // sampler then serves textures of any size, and per-texture values
        // (which would defeat the deduplication of Sampler_Cache) are
        // never needed. A finite value only makes sense to cap detail on
        // purpose.
        float max_lod = VK_LOD_CLAMP_NONE;

        bool operator==(const Sampler_Desc& _other) const
        {
            return mag_filter == _other.mag_filter
                && min_filter == _other.min_filter
                && mipmap_mode == _other.mipmap_mode
                && address_mode_u == _other.address_mode_u
                && address_mode_v == _other.address_mode_v
                && anisotropy == _other.anisotropy
                && max_lod == _other.max_lod;
        }
    };

    // Hash functor for Sampler_Desc, required to use it as an
    // unordered_map key. Folds each field with the shared
    // CoreTypes::Hash_combine (one implementation for the whole engine,
    // with a mixing constant that matches the width of size_t).
    //
    // MUST agree field-for-field with Sampler_Desc::operator==.
    struct Sampler_Desc_Hash
    {
        size_t operator()(const Sampler_Desc& _desc) const
        {
            size_t seed = 0;

            CoreTypes::Hash_combine_value(seed, static_cast<int>(_desc.mag_filter));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_desc.min_filter));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_desc.mipmap_mode));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_desc.address_mode_u));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_desc.address_mode_v));
            CoreTypes::Hash_combine_value(seed, _desc.anisotropy);
            CoreTypes::Hash_combine_value(seed, _desc.max_lod);

            return seed;
        }
    };

    // Sampler_Cache: deduplicates VkSampler objects by configuration.
    //
    // Textures rarely need unique sampling settings — most share the same
    // "linear filtering, repeat wrap, full anisotropy" configuration.
    // Creating a new VkSampler per texture would waste samplers (Vulkan
    // implementations cap maxSamplerAllocationCount) and, for bindless,
    // would bloat the descriptor array with redundant sampler entries.
    //
    // Get_sampler() returns an existing handle if a sampler with the same
    // Sampler_Desc was already created, or creates and caches a new one.
    //
    // Lifetime: owned by Renderer, destroyed at shutdown. All samplers it
    // creates are destroyed together when the cache itself is destroyed —
    // individual samplers are never destroyed early, since multiple
    // textures may reference the same one.
    class Sampler_Cache
    {
    public:

        explicit Sampler_Cache(const Vulkan_Device& _device);
        ~Sampler_Cache();

        Sampler_Cache(const Sampler_Cache&) = delete;
        Sampler_Cache& operator=(const Sampler_Cache&) = delete;
        Sampler_Cache(Sampler_Cache&&) = delete;
        Sampler_Cache& operator=(Sampler_Cache&&) = delete;

        // Returns a VkSampler matching _desc, creating one if this exact
        // configuration hasn't been requested before. The returned handle
        // is owned by the cache — never call vkDestroySampler on it directly.
        VkSampler Get_sampler(const Sampler_Desc& _desc);

        // Vulkan parameters of a CoreTypes::Sampler_Preset: the only place
        // where a preset becomes a Sampler_Desc.
        //   Linear_*  - linear mag/min and mip filtering, anisotropy 16.
        //   Nearest_* - nearest mag/min and mip filtering, no anisotropy.
        //   *_Repeat  - REPEAT on U and V.
        //   *_Clamp   - CLAMP_TO_EDGE on U and V.
        // Every preset uses max_lod = VK_LOD_CLAMP_NONE. The requested
        // anisotropy is clamped to the device when the sampler is created,
        // not here, which is why this function needs no instance.
        //
        // Throws std::invalid_argument for Sampler_Preset::Count or any
        // value outside the enum.
        static Sampler_Desc Get_preset_desc(CoreTypes::Sampler_Preset _preset);

    private:

        VkSampler Create_sampler(const Sampler_Desc& _desc) const;

        VkDevice device_handle;

        // Whether the samplerAnisotropy FEATURE was enabled on the device.
        // The limit alone (maxSamplerAnisotropy) says nothing about the
        // feature: creating a sampler with anisotropyEnable on a device
        // that did not enable it is invalid usage.
        bool     anisotropy_enabled;
        float    max_supported_anisotropy;

        std::unordered_map<Sampler_Desc, VkSampler, Sampler_Desc_Hash> cache;
    };

} // namespace Renderer