#pragma once

#include <Vector.hpp>

#include <array>
#include <cstdint>

namespace CoreTypes
{

    // =========================================================
    // Vertex_Simple
    // =========================================================

    // Minimal vertex: position + color, no lighting information.
    // Use for: debug geometry (gizmos, wireframes, bounding boxes),
    // 2D overlays, or any geometry that does not need lighting.
    struct Vertex_Simple
    {
        MathLib::Vector3 position;
        MathLib::Vector3 color;
    };

    // =========================================================
    // Vertex_Static_Mesh
    // =========================================================

    // Full-featured vertex for static (non-animated) geometry, ready
    // for PBR lighting with normal mapping and per-vertex tint.
    // Use for: regular 3D models, props, environment geometry — anything
    // that does not need skeletal animation.
    //
    // tangent.xyz = tangent direction in world/model space.
    // tangent.w   = bitangent sign (+1 or -1); used in the shader as:
    //   bitangent = cross(normal, tangent.xyz) * tangent.w
    // color       = per-vertex tint, defaults to (1,1,1,1) when unused.
    struct Vertex_Static_Mesh
    {
        MathLib::Vector3 position;
        MathLib::Vector3 normal;
        MathLib::Vector4 tangent;
        MathLib::Vector2 uv;
        MathLib::Vector4 color;
    };

    // =========================================================
    // Vertex_Skinned_Mesh
    // =========================================================

    // Extends Vertex_Static_Mesh with bone influences for skeletal
    // animation. Up to 4 bones can influence a vertex — a common limit
    // that balances visual quality with shader performance.
    // bone_weights must sum to 1.0 across the 4 components.
    //
    // Not yet used (no animation system exists), but defined now so
    // the vertex layout does not need to change when skeletal animation
    // is implemented in Phase 7.
    struct Vertex_Skinned_Mesh
    {
        MathLib::Vector3        position;
        MathLib::Vector3        normal;
        MathLib::Vector4        tangent;
        MathLib::Vector2        uv;
        MathLib::Vector4        color;
        std::array<uint32_t, 4> bone_indices;
        MathLib::Vector4        bone_weights;
    };

} // namespace CoreTypes