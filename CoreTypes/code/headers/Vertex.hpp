#pragma once

#include <Vector.hpp>

#include <array>
#include <cstddef>
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
    // Vertex_Static_Mesh (GPU layout, 32 bytes)
    // =========================================================

    // Vertex for static (non-animated) geometry, ready for PBR lighting
    // with normal mapping and per-vertex tint, exactly as the vertex
    // buffer stores it. Vulkan_Vertex_Layout maps each member to the
    // format below and the vertex fetch converts it back to float, so
    // the shader inputs stay vec3 / vec3 / vec4 / vec2 / vec4.
    //
    //   offset size member    Vulkan format             shader input
    //    0     12  position  R32G32B32_SFLOAT          vec3
    //   12      4  normal    A2B10G10R10_SNORM_PACK32  vec3 (w unused)
    //   16      4  tangent   A2B10G10R10_SNORM_PACK32  vec4 (w = bitangent sign, exactly +-1)
    //   20      8  uv        R32G32_SFLOAT             vec2
    //   28      4  color     R8G8B8A8_UNORM            vec4
    //
    // normal and tangent are one 32-bit word each: x in bits 0-9,
    // y in 10-19, z in 20-29, w in 30-31. color is four bytes in memory
    // order R, G, B, A. uv stays 32-bit: half floats lose too much
    // precision on tiled UVs, and UNORM would need a per-mesh range.
    //
    // Never filled by hand: build a Vertex_Static_Mesh_CPU and convert
    // it with Vertex_Packing::Pack (Mesh_GPU does it at upload).
    struct Vertex_Static_Mesh
    {
        MathLib::Vector3       position;
        uint32_t               normal;
        uint32_t               tangent;
        MathLib::Vector2       uv;
        std::array<uint8_t, 4> color;
    };

    // =========================================================
    // Vertex_Skinned_Mesh (GPU layout, 44 bytes)
    // =========================================================

    // Vertex_Static_Mesh plus bone influences for skeletal animation.
    // Up to 4 bones can influence a vertex.
    //
    //   offset size member        Vulkan format        shader input
    //   32      8  bone_indices  R16G16B16A16_UINT    uvec4
    //   40      4  bone_weights  R8G8B8A8_UNORM       vec4 (sums to exactly 1.0)
    //
    // 16-bit indices because glTF allows up to 65535 joints per skin;
    // 8-bit would need a per-mesh bone remap.
    //
    // Not yet used (no animation system exists), but defined now so
    // the vertex layout does not need to change when skeletal animation
    // is implemented in Phase 7.
    struct Vertex_Skinned_Mesh
    {
        MathLib::Vector3        position;
        uint32_t                normal;
        uint32_t                tangent;
        MathLib::Vector2        uv;
        std::array<uint8_t, 4>  color;
        std::array<uint16_t, 4> bone_indices;
        std::array<uint8_t, 4>  bone_weights;
    };

    // The offsets are part of the contract with Vulkan_Vertex_Layout and
    // the shaders. This fails the build if anything shifts them, e.g.
    // GLM's aligned types turning Vector3 into 16 bytes.
    static_assert(sizeof(Vertex_Static_Mesh) == 32, "Vertex_Static_Mesh must be 32 bytes, no padding");
    static_assert(offsetof(Vertex_Static_Mesh, normal) == 12);
    static_assert(offsetof(Vertex_Static_Mesh, tangent) == 16);
    static_assert(offsetof(Vertex_Static_Mesh, uv) == 20);
    static_assert(offsetof(Vertex_Static_Mesh, color) == 28);

    static_assert(sizeof(Vertex_Skinned_Mesh) == 44, "Vertex_Skinned_Mesh must be 44 bytes, no padding");
    static_assert(offsetof(Vertex_Skinned_Mesh, bone_indices) == 32);
    static_assert(offsetof(Vertex_Skinned_Mesh, bone_weights) == 40);

    // =========================================================
    // Vertex_Static_Mesh_CPU
    // =========================================================

    // Full-precision version of Vertex_Static_Mesh: the format the
    // ResourceManager produces and works on. Mesh_Loader fills it,
    // Primitive_Builder computes tangents on it and Mesh_Optimizer
    // deduplicates and reorders it, all of which need plain floats.
    // MeshData stores it; Mesh_GPU packs it into Vertex_Static_Mesh.
    //
    // tangent.xyz = tangent direction in world/model space.
    // tangent.w   = bitangent sign (+1 or -1); used in the shader as:
    //   bitangent = cross(normal, tangent.xyz) * tangent.w
    // color       = per-vertex tint, defaults to (1,1,1,1) when unused.
    struct Vertex_Static_Mesh_CPU
    {
        MathLib::Vector3 position;
        MathLib::Vector3 normal;
        MathLib::Vector4 tangent;
        MathLib::Vector2 uv;
        MathLib::Vector4 color;
    };

    // =========================================================
    // Vertex_Skinned_Mesh_CPU
    // =========================================================

    // Full-precision version of Vertex_Skinned_Mesh.
    // bone_weights must sum to 1.0 across the 4 components.
    struct Vertex_Skinned_Mesh_CPU
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