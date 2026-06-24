#pragma once

#include <Vector.hpp>
#include <Matrix.hpp>

#include <cstdint>
#include <vector>

namespace CoreTypes
{

    // =========================================================
    // RenderView
    // =========================================================

    // Camera and projection data for a single frame.
    // view_projection is precomputed in the extract to avoid
    // recomputing it per draw call in the Renderer.
    struct RenderView
    {
        MathLib::Matrix4 view;
        MathLib::Matrix4 projection;
        MathLib::Matrix4 view_projection;   // projection * view, precomputed
        MathLib::Vector3 camera_position;
        float            near_plane = 0.1f;
        float            far_plane = 1000.0f;
    };

    // =========================================================
    // Draw_Item
    // =========================================================

    // A single renderable object, fully resolved and ready for the
    // Renderer to consume without touching the ECS or ResourceManager.
    //
    // sort_key: 64-bit packed key computed in the extract.
    // Layout (low → high bits):
    //   [0  - 7 ] material_id  (8  bits, 256  materials max)
    //   [8  - 15] pipeline_id  (8  bits, 256  pipelines max)
    //   [16 - 31] mesh_gpu_id  (16 bits, 65 k meshes max)
    //   [32 - 63] depth_bits   (32 bits, float reinterpreted as uint)
    //             For opacos:      raw float bits (near → far, front-to-back)
    //             For transparents: ~float bits  (inverted = back-to-front)
    //
    // Sorting the opaque/transparent arrays by sort_key ascending
    // naturally groups by pipeline → material → mesh → depth.
    // The Renderer calls std::sort once per array and iterates linearly.
    //
    // pass_mask: bitmask of render passes this item participates in.
    //   Bit 0 = shadow pass
    //   Bit 1 = gbuffer / forward pass
    //   Bit 2 = (reserved)
    //   ...
    // transform_idx: index into RenderPacket::transforms[].
    struct Draw_Item
    {
        uint32_t mesh_gpu_id;
        uint32_t material_id;
        uint32_t transform_idx;
        uint8_t  pass_mask;
        uint64_t sort_key;
    };

    // =========================================================
    // GPU_Light
    // =========================================================

    // Light data already converted to GPU-friendly layout.
    // Directional lights: position_or_direction = direction (normalized).
    // Point/spot lights:  position_or_direction = world position.
    // The shader selects behavior via `type`.
    struct GPU_Light
    {
        MathLib::Vector3 position_or_direction;
        float            intensity = 1.0f;
        MathLib::Vector3 color;
        uint8_t          type = 0;   // 0=directional, 1=point, 2=spot
        float            range = 0.0f;
        float            inner_angle = 0.0f;
        float            outer_angle = 0.0f;
    };

    // =========================================================
    // RenderPacket
    // =========================================================

    // The complete description of a frame to render, produced once
    // per frame by the extract phase in EngineCore and consumed
    // read-only by the Renderer.
    //
    // This struct is AUTOCONTAINED: after the extract writes it,
    // the Renderer can run without touching the ECS or any other
    // engine subsystem. All handles are already resolved to gpu_ids.
    //
    // transforms: pointer to a per-frame flat array of model matrices.
    // Draw_Item::transform_idx indexes into this array.
    // The array is owned by EngineCore (allocated per frame slot) and
    // is guaranteed to outlive the Renderer's consumption of the packet
    // (lockstep now; double-buffered when pipelining is introduced).
    //
    // opaque_items / transparent_items: sorted by sort_key ascending
    // before being handed to the Renderer (done in the extract).
    struct RenderPacket
    {
        RenderView                  view;

        std::vector< Draw_Item >    opaque_items;       // sorted front-to-back
        std::vector< Draw_Item >    transparent_items;  // sorted back-to-front

        std::vector< GPU_Light >    lights;

        // Flat array of model matrices, indexed by Draw_Item::transform_idx.
        // Pointer + count instead of std::vector to avoid an extra copy —
        // the buffer lives in the per-frame slot owned by EngineCore.
        MathLib::Matrix4* transforms = nullptr;
        uint32_t                    transform_count = 0;
    };

    // =========================================================
    // sort_key helpers
    // =========================================================

    // Packs the components of a sort key into a single uint64_t.
    // Call this from the extract when building each Draw_Item.
    // depth_bits: reinterpret_cast<uint32_t>(depth) for opaques,
    //             ~reinterpret_cast<uint32_t>(depth) for transparents.
    inline uint64_t Make_sort_key(uint8_t  _material_id,
        uint8_t  _pipeline_id,
        uint16_t _mesh_gpu_id,
        uint32_t _depth_bits)
    {
        return  static_cast<uint64_t>(_material_id)
            | (static_cast<uint64_t>(_pipeline_id) << 8)
            | (static_cast<uint64_t>(_mesh_gpu_id) << 16)
            | (static_cast<uint64_t>(_depth_bits) << 32);
    }

} // namespace CoreTypes