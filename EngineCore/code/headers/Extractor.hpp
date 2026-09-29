#pragma once

#include <Alpha_Mode.hpp>
#include <Entity.hpp>
#include <RenderPacket.hpp>
#include <Matrix.hpp>

#include <cstdint>
#include <unordered_set>
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

        // Alpha mode of every registered material slot, indexed by slot:
        // the mode each slot was registered with, maintained by whoever
        // registers materials (Engine::Register_material). The pass of an
        // item is looked up here by the slot it draws with, so the pass and
        // the shading always come from the same registered material. A
        // slot outside the table, or a null table, is treated as Opaque.
        // Read-only for the Extractor; must stay alive while Extract runs.
        const std::vector<CoreTypes::Alpha_Mode>* material_alpha_modes = nullptr;
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
    //      transform, plus the clear color, the near plane distance and the
    //      world space culling planes (CoreTypes::Frustum), built from the
    //      camera parameters and the same basis as the view matrix.
    //   2. Iterates entities with Transform_Component + Mesh_Component,
    //      resolves Asset_Handle -> gpu_id, reads the optional
    //      Material_Component's registered slot and fills the transforms.
    //      The item goes to the opaque or the transparent list according
    //      to the alpha mode of that slot (Extract_Params::
    //      material_alpha_modes), never to fields of the component that
    //      may have changed since it was registered. An unregistered
    //      material draws with Default_Material in the opaque pass, and is
    //      reported once per entity. Textures are not resolved here: the
    //      Engine did it once, when it registered the material.
    //   3. Iterates entities with Transform_Component + Light_Component,
    //      fills the GPU_Light array from WORLD positions and directions.
    //   4. Sorts both item lists by sort_key: grouped by pipeline, then by
    //      winding (objects whose transform inverts it, such as a negative
    //      scale on one axis, sit together), material and mesh,
    //      front-to-back inside each group. Equal keys are ordered by
    //      Draw_Item::stable_id (the entity index), so the order is the
    //      same every frame. Transparent items need no back-to-front order:
    //      the Renderer composites them with weighted blended
    //      order-independent transparency.
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

        // Set once a material selecting a sampler preset that does not
        // exist has been reported; such materials fall back to the default
        // preset silently afterwards.
        bool warned_invalid_sampler = false;

        // Entities already reported for carrying a Material_Component that
        // was never registered. The full Entity (index and generation) is
        // stored, so a recycled index is reported again for its new owner.
        std::unordered_set<ECS::Entity> warned_unregistered_material;
    };

} // namespace EngineCore
