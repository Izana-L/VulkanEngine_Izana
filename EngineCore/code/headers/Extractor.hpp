#pragma once

#include <RenderPacket.hpp>
#include <Matrix.hpp>

#include <cstdint>
#include <vector>

namespace ECS { class World; }
namespace ResourceManager { class Resource_Manager; }

namespace EngineCore
{

    // Frame-constant inputs of the extract that come from outside the ECS.
    struct Extract_Params
    {
        // Viewport width / height, derived from the Renderer's render size
        // (the swapchain extent) so it matches the viewport the frame is
        // drawn with. The Camera_Component's aspect_ratio overrides it
        // when non-zero.
        float   aspect_ratio = 1.0f;

        // Pipeline ids handed out by the Renderer, packed into sort keys.
        uint8_t opaque_pipeline_id = 0;
        uint8_t transparent_pipeline_id = 0;
    };

    // Extractor: reads the ECS state once per frame and produces a
    // self-contained RenderPacket that the Renderer can consume without
    // any knowledge of the ECS or ResourceManager.
    //
    // Called by EngineCore after Transform_System::Update() and before
    // Renderer::Render(). The packet is valid until the next Extract() call.
    //
    // What it does each frame:
    //   1. Finds the active camera (lowest render_order Camera_Component
    //      with is_active=true) and builds the RenderView from its WORLD
    //      transform, plus the clear color.
    //   2. Iterates entities with Transform_Component + Mesh_Component,
    //      resolves Asset_Handle -> gpu_id, reads the optional
    //      Material_Component (tint, albedo texture), routes each item to
    //      the opaque or the transparent list and fills the transforms.
    //   3. Iterates entities with Transform_Component + Light_Component,
    //      fills the GPU_Light array from WORLD positions and directions.
    //   4. Sorts opaque items front-to-back and transparent items
    //      back-to-front by sort_key.
    //
    // Everything spatial is taken from Transform_Component::world_matrix,
    // never from the local position/rotation: a camera or a light parented
    // to a moving pivot follows it exactly like a mesh does.
    //
    // Returns false if no active camera is found; the loop should skip
    // Renderer::Render() that frame.
    class Extractor
    {
    public:

        Extractor() = default;
        ~Extractor() = default;

        Extractor(const Extractor&) = delete;
        Extractor& operator=(const Extractor&) = delete;

        // Fills _out_packet from the current ECS state.
        // Returns false if no active camera exists (caller skips Render).
        bool Extract(const ECS::World& _world,
            const ResourceManager::Resource_Manager& _resources,
            const Extract_Params& _params,
            CoreTypes::RenderPacket& _out_packet);

    private:

        // Per-frame transform buffer. Cleared and refilled each Extract().
        // RenderPacket::transforms points into this vector: valid until
        // the next Extract() call.
        std::vector<MathLib::Matrix4> transform_buffer;
    };

} // namespace EngineCore
