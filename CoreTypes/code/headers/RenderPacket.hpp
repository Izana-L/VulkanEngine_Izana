#pragma once

#include <Vector.hpp>
#include <Matrix.hpp>
#include <Sampler_Preset.hpp>
#include <MathConstants.hpp>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace CoreTypes
{

    // =========================================================
    // Frustum
    // =========================================================

    // The planes an object is culled against, in WORLD space.
    //
    // Each plane stores its normal pointing INSIDE the frustum in xyz and
    // its offset in w: a point p is on the inner side when
    // dot(xyz, p) + w >= 0. The Extractor stores unit normals, which makes
    // that value the signed distance to the plane; the culling test below
    // does not depend on it.
    //
    // Five planes: left, right, bottom, top and near. There is no far
    // plane: the perspective projection is infinite. An orthographic
    // camera does have a far plane, which is left to the rasterizer: the
    // culling stays conservative.
    //
    // A default-constructed Frustum (all planes zero) contains everything,
    // so a view that never filled it culls nothing.
    //
    // Built by the Extractor from the camera parameters; the Renderer
    // uploads the same values for the GPU culling (cull_objects.comp) and
    // tests the transparent items against them on the CPU, both with the
    // bounding ellipsoid test below.
    struct Frustum
    {
        static constexpr uint32_t PLANE_COUNT = 5;

        std::array<MathLib::Vector4, PLANE_COUNT> planes{};

        // Half-width, measured along _normal, of the ellipsoid that a local
        // sphere of radius _local_radius becomes under _model: with M the
        // upper 3x3 of _model (columns M0, M1, M2),
        //   extent = _local_radius * |M^T n|,  M^T n = (M0.n, M1.n, M2.n).
        // Exact for any affine matrix (non-uniform scale, shear from a
        // rotated child under a non-uniformly scaled parent, reflections).
        // Scales with |_normal| like the signed distance does.
        //
        // Mirrored by Bounding_ellipsoid_extent in mesh_table.glsl: the CPU
        // and the GPU culling evaluate the same formula.
        static float Ellipsoid_extent(const MathLib::Matrix4& _model, float _local_radius, const MathLib::Vector3& _normal)
        {
            const MathLib::Vector3 transposed_normal(glm::dot(MathLib::Vector3(_model[0]), _normal),
                                                     glm::dot(MathLib::Vector3(_model[1]), _normal),
                                                     glm::dot(MathLib::Vector3(_model[2]), _normal));

            return _local_radius * glm::length(transposed_normal);
        }

        // False when the bounding volume of a mesh lies entirely on the
        // outer side of one plane. The volume is the mesh's local bounding
        // sphere (_local_sphere: xyz = center, w = radius, mesh space)
        // placed by _model, which is an ellipsoid; it is tested directly,
        // never replaced by an enclosing world sphere: for every plane,
        //   d = dot(n, M * c + t) + w       signed distance of the center,
        //   r = Ellipsoid_extent(n)          half-width along n,
        // and the object is outside when d < -r. A volume crossing a plane
        // (partly visible) is kept. The test does not require unit
        // normals: d and r scale alike.
        //
        // No world space sphere comes out of it. A consumer that needs one
        // (occlusion against a depth pyramid, level of detail from the
        // projected size) needs a conservative bound of the scale of the
        // 3x3, e.g. Gershgorin on M^T M.
        bool Intersects_ellipsoid(const MathLib::Matrix4& _model, const MathLib::Vector4& _local_sphere) const
        {
            const MathLib::Vector3 center = MathLib::Vector3(_model * MathLib::Vector4(MathLib::Vector3(_local_sphere), 1.0f));

            for (const MathLib::Vector4& plane : planes)
            {
                const MathLib::Vector3 normal(plane);

                if (glm::dot(normal, center) + plane.w < -Ellipsoid_extent(_model, _local_sphere.w, normal))
                    return false;
            }

            return true;
        }
    };

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

        // Distance from the eye to the near plane: where the depth slices of
        // the clustered lighting start.
        float            near_plane = MathLib::Constants::NEAR_PLANE_DEFAULT;

        // Culling planes of the camera (see Frustum).
        Frustum          frustum;
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

    // Bindless texture slots reserved for the default textures. The
    // Renderer uploads them at startup, before any other texture, so each
    // value below IS its slot. They make every texture index a Draw_Item
    // carries point to a written slot:
    //   Error       - magenta (255, 0, 255): the material references an
    //                 image with no GPU index. Meant to be noticed.
    //   White       - (255, 255, 255): legitimate absence of albedo (with
    //                 PBR, also of metallic-roughness and AO, so the
    //                 scalar factors decide).
    //   Black       - (0, 0, 0): absent emissive.
    //   Flat_Normal - (128, 128, 255), UNORM rather than sRGB: flat
    //                 tangent-space normal for an absent normal map.
    // All four are 1x1 and opaque.
    namespace Default_Texture
    {
        inline constexpr uint32_t Error = 0;
        inline constexpr uint32_t White = 1;
        inline constexpr uint32_t Black = 2;
        inline constexpr uint32_t Flat_Normal = 3;
        inline constexpr uint32_t Count = 4;
    }

    // Slot of the material table reserved for the default material. The
    // Renderer registers it at startup, before any other material, so it
    // is always slot 0: white base color, Default_Texture::White albedo,
    // Sampler_Preset::Linear_Repeat. An item without a material draws with
    // it, which looks exactly like an untextured, untinted draw.
    inline constexpr uint32_t Default_Material = 0;

    // =========================================================
    // Draw_Item
    // =========================================================

    // sort_key: 64-bit packed key computed in the extract.
    //
    // Layout (HIGH -> low bits). The order matters: an ascending sort of a
    // uint64 is dominated by the most significant bits, so whatever sits
    // highest is the primary grouping.
    //   [56 - 63] pipeline_id  (8  bits, 256  pipelines max)  <- primary
    //   [48 - 55] material_id  (8  bits, 256  materials max), whose top bit
    //             (Sort_Key_Mirrored_Bit) is the winding hint below
    //   [32 - 47] mesh_gpu_id  (16 bits, 65 k meshes max)
    //   [0  - 31] depth_bits   (32 bits, float made sortable as uint):
    //             Depth_to_sortable_bits (near -> far), for both lists
    //
    // Sorting ascending therefore groups by pipeline -> winding -> material
    // -> mesh, and sorts by depth WITHIN each group.
    //
    // Winding hint: the objects whose transform inverts the winding
    // (Inverts_winding) are drawn with the opposite front face, and every
    // change of front face is a state change (and, on the CPU-indirect path,
    // a new indirect draw). The Extractor sets the top bit of the material
    // field for them, so they end up next to each other right below their
    // pipeline. It is only an ordering hint: the Renderer looks at the
    // transform itself to decide the winding, and a wrong or missing hint
    // costs state changes, never correctness.
    //
    // The trade-off, stated plainly: depth is not the primary sort, so
    // front-to-back ordering is per-batch instead of global and some overdraw
    // comes back. That is the standard choice: a pipeline bind costs far
    // more than the overdraw it saves.
    //
    // Transparent items need no back-to-front order either: the Renderer
    // composites them with weighted blended order-independent transparency,
    // whose result does not depend on the draw order. A global back-to-front
    // order would not be enough anyway: it cannot resolve objects that
    // intersect, contain one another or overlap cyclically. Their depth
    // bits only keep the order deterministic from frame to frame (the
    // half-precision accumulation rounds differently in another order).
    //
    // The rest of the material_id field of the key is still 0: grouping draws
    // by material only pays off once materials bind per-draw state, and every
    // material is now read from one table (Draw_Item::material_index).
    struct Draw_Item
    {
        uint32_t         mesh_gpu_id = 0;
        uint32_t         transform_idx = 0;

        // Slot of the item's material in the Renderer's material table
        // (base color, albedo texture and sampler). Never an unregistered
        // slot: an item without a material uses Default_Material. The
        // Renderer copies it into the object buffer, and the shaders read
        // the material from there.
        uint32_t         material_index = Default_Material;

        // Which passes draw this item (Render_Pass_Bit).
        uint8_t          pass_mask = Render_Pass_Bit::Opaque;

        uint64_t         sort_key = 0;
    };

     // =========================================================
     // GPU_Light
     // =========================================================

     // One entry of the light storage buffer, in its final GPU form. The
     // Extractor fills it and the Renderer copies the packet's array into
     // the mapped buffer as it is: there is no second struct in between.
     //
     // EXACT mirror of `Light` in Renderer/shaders/common/frame_set.glsl
     // (std430, 64 bytes). A field added here is added there, in the same
     // position; the static_asserts below pin the offsets the shader reads.
     //
     // Directional lights: position_or_direction = direction the light POINTS TO (normalized).
     // Point/spot lights:  position_or_direction = world position.
     // The shader selects behavior via `type`.
    struct GPU_Light
    {
        MathLib::Vector3 position_or_direction;
        float            intensity = 1.0f;
        MathLib::Vector3 color;
        float            range = 0.0f;
        MathLib::Vector3 spot_direction = { 0.0f, 0.0f, -1.0f };
        float            inner_angle = 0.0f;
        float            outer_angle = 0.0f;
        int32_t          type = 0;            // 0=directional, 1=point, 2=spot
        float            _padding0 = 0.0f;
        float            _padding1 = 0.0f;
    };

    static_assert(sizeof(GPU_Light) == 64, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(GPU_Light, position_or_direction) == 0, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(GPU_Light, intensity) == 12, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(GPU_Light, color) == 16, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(GPU_Light, range) == 28, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(GPU_Light, spot_direction) == 32, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(GPU_Light, inner_angle) == 44, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(GPU_Light, outer_angle) == 48, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(offsetof(GPU_Light, type) == 52, "GPU_Light breaks the std430 layout of frame_set.glsl");
    static_assert(std::is_trivially_copyable_v<GPU_Light>, "GPU_Light is copied with memcpy");

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
    // before being handed to the Renderer (done in the extract): grouped by
    // pipeline, material and mesh, front-to-back inside each group.
    struct RenderPacket
    {
        RenderView                  view;

        // Background color the framebuffer is cleared to, linear RGBA.
        // Comes from the active Camera_Component.
        MathLib::Vector4            clear_color = { 0.01f, 0.01f, 0.01f, 1.0f };

        std::vector< Draw_Item >    opaque_items;       // grouped, front-to-back inside each group
        std::vector< Draw_Item >    transparent_items;  // grouped like the opaque ones; composited order-independently

        std::vector< GPU_Light >    lights;
        uint32_t                    directional_light_count = 0;

        // Flat array of model matrices, indexed by Draw_Item::transform_idx.
        // Pointer + count instead of std::vector to avoid an extra copy:
        // the buffer lives in the per-frame slot owned by EngineCore.
        const MathLib::Matrix4*     transforms = nullptr;
        uint32_t                    transform_count = 0;
    };

    // =========================================================
    // Winding
    // =========================================================

    // True when _model inverts the winding of the triangles: the
    // determinant of its upper 3x3 (the triple product of its columns) is
    // negative, as with a negative scale on an odd number of axes or a
    // reflection. Such an object is drawn with the opposite front face, or
    // it would show its inside. A zero determinant (a collapsed object)
    // draws nothing, so its winding does not matter.
    //
    // The Renderer decides the winding of every object with this function
    // (Draw_List_Builder); the Extractor uses it for the ordering hint of
    // the sort key.
    inline bool Inverts_winding(const MathLib::Matrix4& _model)
    {
        const MathLib::Vector3 column_x(_model[0]);
        const MathLib::Vector3 column_y(_model[1]);
        const MathLib::Vector3 column_z(_model[2]);

        return glm::dot(glm::cross(column_x, column_y), column_z) < 0.0f;
    }

    // =========================================================
    // sort_key helpers
    // =========================================================

    // Top bit of the material_id field of a sort key: the object's
    // transform inverts the winding. Groups the mirrored objects of a
    // pipeline together (see the layout of the key).
    inline constexpr uint8_t Sort_Key_Mirrored_Bit = 0x80;

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

    static_assert(Depth_to_sortable_bits(-100.0f) < Depth_to_sortable_bits(-2.0f));
    static_assert(Depth_to_sortable_bits(-2.0f) < Depth_to_sortable_bits(0.0f));
    static_assert(Depth_to_sortable_bits(0.0f) < Depth_to_sortable_bits(2.0f));
    static_assert(Depth_to_sortable_bits(2.0f) < Depth_to_sortable_bits(100.0f));

} // namespace CoreTypes
