#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Render_Pass.hpp>
#include <Vulkan_Pipeline.hpp>
#include <Hash.hpp>
#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Renderer_System
{

    // Hash functor for Pipeline_Config, so it can be an unordered_map key.
    // Uses the shared CoreTypes::Hash_combine, like Sampler_Desc_Hash.
    //
    // MUST agree field-for-field with Pipeline_Config::operator==.
    struct Pipeline_Config_Hash
    {
        size_t operator()(const Pipeline_Config& _config) const
        {
            size_t seed = 0;

            CoreTypes::Hash_combine_value(seed, _config.vertex_shader_path);
            CoreTypes::Hash_combine_value(seed, _config.fragment_shader_path);
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_config.polygon_mode));
            CoreTypes::Hash_combine_value(seed, _config.blend_enable);
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_config.src_color_blend_factor));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_config.dst_color_blend_factor));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_config.color_blend_op));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_config.src_alpha_blend_factor));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_config.dst_alpha_blend_factor));
            CoreTypes::Hash_combine_value(seed, static_cast<int>(_config.alpha_blend_op));

            return seed;
        }
    };

    // Pipeline_Registry: deduplicates VkPipeline objects by configuration,
    // exactly as Sampler_Cache does for VkSampler.
    //
    // This is what the pipeline cache of Paso 1 was waiting for: with one
    // pipeline there was nothing to deduplicate and nothing to warm up.
    // Every pipeline it builds goes through the shared VkPipelineCache, so
    // from the second run on they come back compiled.
    //
    // Lifetime: owned by Renderer. Every pipeline is destroyed together when
    // the registry is — none is ever destroyed early, since any number of
    // draw batches may reference the same one.
    class Pipeline_Registry
    {
        const Vulkan_Device& device;
        const Vulkan_Render_Pass& render_pass;
        VkPipelineCache           pipeline_cache;   // not owned
        VkPipelineLayout          shared_layout;    // not owned

        // One entry per distinct pipeline. The id is a small dense integer
         // assigned on creation — it is what travels inside the sort key,
         // because a uint8 fits in 8 bits and a Pipeline_Config does not.
        struct Entry
        {
            Vulkan_Pipeline pipeline;
            uint8_t         id;

            Entry(const Vulkan_Device& _device,
                const Vulkan_Render_Pass& _render_pass,
                VkPipelineCache _cache,
                VkPipelineLayout _layout,
                const Pipeline_Config& _config,
                uint8_t _id)
                : pipeline(_device, _render_pass, _cache, _layout, _config),
                id(_id) {}
        };

        std::unordered_map<Pipeline_Config, Entry, Pipeline_Config_Hash> registry;

        // Flat index: by_id[i] is the handle of the pipeline with id i.
        // Non-owning — the map above owns them. This is the hot-path lookup.
        std::vector<VkPipeline> by_id;
        bool sealed = false;

    public:

        Pipeline_Registry(const Vulkan_Device& _device,
            const Vulkan_Render_Pass& _render_pass,
            VkPipelineCache _pipeline_cache,
            VkPipelineLayout _shared_layout);

        ~Pipeline_Registry() = default;

        Pipeline_Registry(const Pipeline_Registry&) = delete;
        Pipeline_Registry& operator=(const Pipeline_Registry&) = delete;
        Pipeline_Registry(Pipeline_Registry&&) = delete;
        Pipeline_Registry& operator=(Pipeline_Registry&&) = delete;

        void Warm_up(const std::vector<Pipeline_Config>& _manifest);

        // True once Warm_up has run. Nothing should be built after this.
        bool Is_sealed() const { return sealed; }

        // LOAD TIME. Returns the stable id for _config, building the
        // pipeline if this configuration is new. Hashes two std::strings:
        // fine once at setup, wrong once per draw.
        //
        // Building after Warm_up() is a frame hitch, not a fault: the
        // pipeline is built and a warning names the missing manifest
        // entry, in every build configuration. Throws std::length_error
        // when the 256-pipeline limit of the sort key is reached.
        uint8_t Get_id(const Pipeline_Config & _config);

        // HOT PATH. O(1) array index, no hashing, no allocation.
        // _id must have come from Get_id().
        VkPipeline Get_by_id(uint8_t _id) const;

        // How many distinct pipelines exist — the number Paso 4 warms up.
        size_t Size() const { return registry.size(); }
    };

} // namespace Renderer_System