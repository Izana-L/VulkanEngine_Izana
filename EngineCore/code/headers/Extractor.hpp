#pragma once

#include <RenderPacket.hpp>
#include <Matrix.hpp>

#include <vector>

namespace ECS { class World; }
namespace ResourceManager { class Resource_Manager; }

namespace EngineCore
{

    // Extractor: reads the ECS state once per frame and produces a
    // self-contained RenderPacket that the Renderer can consume without
    // any knowledge of the ECS or ResourceManager.
    //
    // Called by EngineCore after Transform_System::Update() and before
    // Renderer::Render(). The packet is valid until the next Extract() call.
    //
    // What it does each frame:
    //   1. Finds the active camera (lowest render_order Camera_Component
    //      with is_active=true) and builds the RenderView.
    //   2. Iterates entities with Transform_Component + Mesh_Component,
    //      resolves Asset_Handle → gpu_id, fills Draw_Items and transforms.
    //   3. Iterates entities with Transform_Component + Light_Component,
    //      fills GPU_Light array.
    //   4. Sorts opaque_items front-to-back by sort_key.
    //
    // Returns false if no active camera is found — EngineCore should skip
    // Renderer::Render() that frame.
    class Extractor
    {
    public:

        Extractor() = default;
        ~Extractor() = default;

        Extractor(const Extractor&) = delete;
        Extractor& operator=(const Extractor&) = delete;

        // Fills _out_packet from the current ECS state.
        // _aspect_ratio: viewport width / height, computed by EngineCore
        //   from the swapchain extent. The Camera_Component's aspect_ratio
        //   field overrides this if non-zero.
        // Returns false if no active camera exists (caller skips Render).
        bool Extract(const ECS::World& _world,const ResourceManager::Resource_Manager& _resources,
                     float _aspect_ratio, CoreTypes::RenderPacket& _out_packet);

    private:

        // Per-frame transform buffer. Cleared and refilled each Extract().
        // RenderPacket::transforms points into this vector — valid until
        // the next Extract() call.
        std::vector<MathLib::Matrix4> transform_buffer;
    };

} // namespace EngineCore