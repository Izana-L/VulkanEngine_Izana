#pragma once

#include <Vector.hpp>
#include <Matrix.hpp>
#include <bit>
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

    // sort_key: 64-bit packed key computed in the extract.
    //
    // Layout (HIGH → low bits). The order matters: an ascending sort of a
    // uint64 is dominated by the most significant bits, so whatever sits
    // highest is the primary grouping.
    //   [56 - 63] pipeline_id  (8  bits, 256  pipelines max)  ← primary
    //   [48 - 55] material_id  (8  bits, 256  materials max)
    //   [32 - 47] mesh_gpu_id  (16 bits, 65 k meshes max)
    //   [0  - 31] depth_bits   (32 bits, float reinterpreted as uint)
    //             For opaques:      raw float bits (near → far)
    //             For transparents: ~float bits   (inverted = far → near)
    //
    // Sorting ascending therefore groups by pipeline → material → mesh, and
    // sorts by depth WITHIN each group.
    //
    // The trade-off, stated plainly: depth is no longer the primary sort, so
    // front-to-back ordering is per-batch instead of global and some overdraw
    // comes back. That is the standard choice — a pipeline bind costs far
    // more than the overdraw it saves — but it IS a choice.
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
        MathLib::Vector3 spot_direction = { 0.0f, 0.0f, -1.0f };
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
    inline uint64_t Make_sort_key(uint8_t  _pipeline_id,
        uint8_t  _material_id,
        uint16_t _mesh_gpu_id,
        uint32_t _depth_bits)
    {
        return  static_cast<uint64_t>(_depth_bits)
            | (static_cast<uint64_t>(_mesh_gpu_id) << 32)
            | (static_cast<uint64_t>(_material_id) << 48)
            | (static_cast<uint64_t>(_pipeline_id) << 56);
    }

    // Unpacks the pipeline id back out of a sort key.
    //
    // Lives next to Make_sort_key on purpose: packing and unpacking must
    // move together. If the bit layout above ever changes again, this is the
    // other half that has to change in the same edit.
    inline uint8_t Get_pipeline_id(uint64_t _sort_key)
    {
        return static_cast<uint8_t>(_sort_key >> 56);
    }
    inline constexpr uint32_t Depth_to_sortable_bits(float _depth)
    {
        const uint32_t bits = std::bit_cast<uint32_t>(_depth);

        return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
    }
    static_assert(Depth_to_sortable_bits(-100.0f) < Depth_to_sortable_bits(-2.0f));
    static_assert(Depth_to_sortable_bits(-2.0f) < Depth_to_sortable_bits(0.0f));
    static_assert(Depth_to_sortable_bits(0.0f) < Depth_to_sortable_bits(2.0f));
    static_assert(Depth_to_sortable_bits(2.0f) < Depth_to_sortable_bits(100.0f));

} // namespace CoreTypes