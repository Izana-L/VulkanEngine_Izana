#pragma once

#include <Vector.hpp>
#include <Matrix.hpp>
#include <Sampler_Preset.hpp>
#include <bit>
#include <cstdint>
#include <vector>

namespace CoreTypes
{

    // =========================================================
    // RenderView
    // =========================================================

    // Camera and projection data for a single frame, all in WORLD space.
    // view_projection is precomputed in the extract to avoid recomputing
    // it per draw call in the Renderer.
    //
    // Only fields with a consumer live here. Inverse matrices, clip planes
    // and clock values were removed when it was established that nothing
    // read them: every field below is either uploaded to the per-frame
    // uniform block (see Renderer_System::Frame_UBO and frame_set.glsl) or
    // used by the extract itself for depth sorting.
    //
    // Reverse-Z is in effect for the projection: the near plane maps to
    // depth 1.0 and the far end to 0.0. Anything reconstructing view-space
    // position or linear depth from the depth buffer must account for that.
    struct RenderView
    {
        MathLib::Matrix4 view;
        MathLib::Matrix4 projection;
        MathLib::Matrix4 view_projection;   // projection * view, precomputed
        MathLib::Vector3 camera_position;   // world-space eye position
        MathLib::Vector3 camera_forward;    // world-space unit forward vector
    };

    // =========================================================
    // Render passes
    // =========================================================

    // Bit mask of the passes a Draw_Item takes part in. The Renderer tests
    // the bit of the pass it is recording before drawing an item, so an
    // item routed to a list it does not belong to is skipped rather than
    // drawn in every pass alike.
    namespace Render_Pass_Bit
    {
        inline constexpr uint8_t Opaque = 1u << 0;
        inline constexpr uint8_t Transparent = 1u << 1;
        inline constexpr uint8_t All = 0xFFu;
    }

    // Value of Draw_Item::albedo_texture_index when the item has no
    // texture. Mirrors INVALID_TEXTURE_INDEX in the fragment shader.
    inline constexpr uint32_t INVALID_TEXTURE_INDEX = 0xFFFFFFFFu;

    // =========================================================
    // Draw_Item
    // =========================================================

    // sort_key: 64-bit packed key computed in the extract.
    //
    // Layout (HIGH -> low bits). The order matters: an ascending sort of a
    // uint64 is dominated by the most significant bits, so whatever sits
    // highest is the primary grouping.
    //   [56 - 63] pipeline_id  (8  bits, 256  pipelines max)  <- primary
    //   [48 - 55] material_id  (8  bits, 256  materials max)
    //   [32 - 47] mesh_gpu_id  (16 bits, 65 k meshes max)
    //   [0  - 31] depth_bits   (32 bits, float made sortable as uint)
    //             For opaques:      Depth_to_sortable_bits (near -> far)
    //             For transparents: Depth_to_sortable_bits_back_to_front
    //
    // Sorting ascending therefore groups by pipeline -> material -> mesh, and
    // sorts by depth WITHIN each group.
    //
    // The trade-off, stated plainly: depth is not the primary sort, so
    // front-to-back ordering is per-batch instead of global and some overdraw
    // comes back. That is the standard choice: a pipeline bind costs far
    // more than the overdraw it saves.
    //
    // The material id lives ONLY inside the key (there is no material table
    // to index yet); Get_material_id() unpacks it when a consumer appears.
    struct Draw_Item
    {
        uint32_t         mesh_gpu_id = 0;
        uint32_t         transform_idx = 0;

        // Bindless index of the albedo texture, or INVALID_TEXTURE_INDEX.
        // Travels to the fragment shader through the push constant block.
        uint32_t         albedo_texture_index = INVALID_TEXTURE_INDEX;

        // Bindless index of the sampler the albedo texture is read with:
        // the material's Sampler_Preset, whose value is its slot in the
        // sampler array. Travels next to albedo_texture_index.
        uint32_t         albedo_sampler_index = static_cast<uint32_t>(Sampler_Preset::Linear_Repeat);

        // Which passes draw this item (Render_Pass_Bit).
        uint8_t          pass_mask = Render_Pass_Bit::Opaque;

        uint64_t         sort_key = 0;

        // Per-draw tint multiplied with the vertex color (and the albedo
        // texture when present). Alpha below 1.0 is what routes an item to
        // the transparent list.
        MathLib::Vector4 base_color = { 1.0f, 1.0f, 1.0f, 1.0f };
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
    // This struct is SELF-CONTAINED: after the extract writes it,
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

        // Background color the framebuffer is cleared to, linear RGBA.
        // Comes from the active Camera_Component.
        MathLib::Vector4            clear_color = { 0.01f, 0.01f, 0.01f, 1.0f };

        std::vector< Draw_Item >    opaque_items;       // sorted front-to-back
        std::vector< Draw_Item >    transparent_items;  // sorted back-to-front

        std::vector< GPU_Light >    lights;

        // Flat array of model matrices, indexed by Draw_Item::transform_idx.
        // Pointer + count instead of std::vector to avoid an extra copy:
        // the buffer lives in the per-frame slot owned by EngineCore.
        const MathLib::Matrix4*     transforms = nullptr;
        uint32_t                    transform_count = 0;
    };

    // =========================================================
    // sort_key helpers
    // =========================================================

    // Packs the components of a sort key into a single uint64_t.
    // Call this from the extract when building each Draw_Item.
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
    // move together. If the bit layout above ever changes, this is the
    // other half that has to change in the same edit.
    inline uint8_t Get_pipeline_id(uint64_t _sort_key)
    {
        return static_cast<uint8_t>(_sort_key >> 56);
    }

    inline uint8_t Get_material_id(uint64_t _sort_key)
    {
        return static_cast<uint8_t>((_sort_key >> 48) & 0xFFu);
    }

    // Maps a float to an unsigned integer whose ordering matches the
    // float's ordering (negative values included), so depths can be sorted
    // as plain integers inside the key.
    inline constexpr uint32_t Depth_to_sortable_bits(float _depth)
    {
        const uint32_t bits = std::bit_cast<uint32_t>(_depth);

        return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
    }

    // Same mapping with the order reversed, for lists drawn back-to-front.
    inline constexpr uint32_t Depth_to_sortable_bits_back_to_front(float _depth)
    {
        return ~Depth_to_sortable_bits(_depth);
    }

    static_assert(Depth_to_sortable_bits(-100.0f) < Depth_to_sortable_bits(-2.0f));
    static_assert(Depth_to_sortable_bits(-2.0f) < Depth_to_sortable_bits(0.0f));
    static_assert(Depth_to_sortable_bits(0.0f) < Depth_to_sortable_bits(2.0f));
    static_assert(Depth_to_sortable_bits(2.0f) < Depth_to_sortable_bits(100.0f));
    static_assert(Depth_to_sortable_bits_back_to_front(100.0f) < Depth_to_sortable_bits_back_to_front(2.0f));

} // namespace CoreTypes
