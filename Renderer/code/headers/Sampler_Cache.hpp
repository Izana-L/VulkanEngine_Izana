#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <cstdint>
#include <unordered_map>

namespace Renderer_System
{

    // Sampler_Desc: the subset of VkSamplerCreateInfo parameters that
    // actually vary across textures in this engine. Used as a hashable,
    // comparable key into Sampler_Cache.
    //
    // Kept deliberately small — only fields we actually configure
    // differently per texture. If a new sampling need arises (e.g.
    // VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT for a specific effect),
    // add the field here rather than creating samplers ad-hoc elsewhere.
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

        // Number of mip levels this sampler can address (maxLod).
        // Must match (or exceed) the highest mip count of any texture
        // that will use this sampler — set to the texture's mip_levels
        // when requesting a sampler for it.
        float max_lod = 0.0f;

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
    // unordered_map key. Combines each field's hash with the classic
    // boost::hash_combine pattern.
    struct Sampler_Desc_Hash
    {
        size_t operator()(const Sampler_Desc& _desc) const
        {
            size_t seed = 0;
            auto combine = [&seed](size_t _value)
                {
                    seed ^= _value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                };

            combine(std::hash<int>{}(static_cast<int>(_desc.mag_filter)));
            combine(std::hash<int>{}(static_cast<int>(_desc.min_filter)));
            combine(std::hash<int>{}(static_cast<int>(_desc.mipmap_mode)));
            combine(std::hash<int>{}(static_cast<int>(_desc.address_mode_u)));
            combine(std::hash<int>{}(static_cast<int>(_desc.address_mode_v)));
            combine(std::hash<float>{}(_desc.anisotropy));
            combine(std::hash<float>{}(_desc.max_lod));

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

        // Convenience: returns the cache's default sampler — linear
        // filtering, repeat wrap, full anisotropy, max_lod set high enough
        // for any reasonable texture (16 levels covers up to 32768px).
        // This is what most PBR textures (albedo, normal, etc.) should use.
        VkSampler Get_default_sampler();

    private:

        VkSampler Create_sampler(const Sampler_Desc& _desc) const;

        VkDevice device_handle;
        float    max_supported_anisotropy;

        std::unordered_map<Sampler_Desc, VkSampler, Sampler_Desc_Hash> cache;
    };

} // namespace Renderer