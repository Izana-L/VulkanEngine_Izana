#include <Pipeline_Registry.hpp>
#include <chrono>
#include <cassert>
#include <iostream>
#include <tuple>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Pipeline_Registry::Pipeline_Registry(const Vulkan_Device& _device,
        const Vulkan_Render_Pass& _render_pass,
        VkPipelineCache _pipeline_cache,
        VkPipelineLayout _shared_layout)
        : device(_device),
        render_pass(_render_pass),
        pipeline_cache(_pipeline_cache),
        shared_layout(_shared_layout)
    {
        assert(shared_layout != VK_NULL_HANDLE &&
            "Pipeline_Registry: shared pipeline layout must be created first");
    }

    uint8_t Pipeline_Registry::Get_id(const Pipeline_Config& _config)
    {
        auto it = registry.find(_config);
        if (it != registry.end())
            return it->second.id;

    
        if (sealed)
        {
            std::cerr << "[Pipeline_Registry] WARNING: building a pipeline "
                "AFTER warm-up — the manifest is incomplete. This is "
                "a frame hitch. Missing config: "
                << _config.vertex_shader_path << " + "
                << _config.fragment_shader_path << "\n";

            assert(false &&"Pipeline_Registry: pipeline built after warm-up — add it to " "Renderer::Build_pipeline_manifest()");
        }

        // 8 bits in the sort key is the hard ceiling. Blowing past it would
        // silently wrap and bind the wrong pipeline, so it fails loudly.
        if (by_id.size() >= 256)
        {
            throw std::runtime_error("Pipeline_Registry: more than 256 pipelines — pipeline_id no " "longer fits in the 8 bits the sort key reserves for it.");
        }

        const uint8_t new_id = static_cast<uint8_t>(by_id.size());

        auto [inserted, ok] = registry.emplace(std::piecewise_construct,std::forward_as_tuple(_config),
                                               std::forward_as_tuple(device, render_pass, pipeline_cache, shared_layout, _config, new_id));
  
        assert(ok && "Pipeline_Registry: emplace failed on a key that wasn't found");

        by_id.push_back(inserted->second.pipeline.Get_handle());

        std::cout << "[Pipeline_Registry] Built pipeline id=" << int(new_id)
            << " (" << registry.size() << " total): "
            << _config.vertex_shader_path << " + "
            << _config.fragment_shader_path << "\n";

        return new_id;
    }

    VkPipeline Pipeline_Registry::Get_by_id(uint8_t _id) const
    {
        assert(_id < by_id.size() &&
            "Pipeline_Registry::Get_by_id: id was never handed out by Get_id");

        return by_id[_id];
    }
    // ---------- Warm_up ----------
    void Pipeline_Registry::Warm_up(const std::vector<Pipeline_Config>& _manifest)
    {
        assert(!sealed && "Pipeline_Registry::Warm_up called more than once");

        const auto start = std::chrono::steady_clock::now();

        for (const Pipeline_Config& config : _manifest)
            Get_id(config);

        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();

        sealed = true;

        // This number is the honest measure of whether Paso 1's cache works.
        // Cold run vs. warm run on the same machine: if the second is not
        // visibly faster, the cache is not being hit — regardless of what
        // the creation-feedback bit claims.
        std::cout << "[Pipeline_Registry] Warm-up complete: "
            << registry.size() << " pipeline(s) in "
            << elapsed_ms << " ms. Registry sealed.\n";
    }
} // namespace Renderer_System