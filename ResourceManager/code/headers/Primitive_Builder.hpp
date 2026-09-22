#pragma once

#include <MeshData.hpp>
#include <Primitive_Desc.hpp>

namespace ResourceManager::Primitive_Builder
{

    // Builds a unit-sized procedural mesh from a Primitive_Desc.
    // Dispatches on the CANONICAL form of the descriptor to the generator
    // below, so the mesh built is exactly the one the cache key describes.
    // All output meshes have positions, normals, UVs and tangents
    // computed, with UINT32 indices.
    //
    // Geometry contract, identical for every generator:
    //   - Triangles are wound COUNTER-CLOCKWISE when seen from outside
    //     (the glTF convention), so every vertex normal points to the
    //     same side as the geometric normal cross(v1 - v0, v2 - v0).
    //     With the Y flip applied by the projection matrix this is what
    //     VK_FRONT_FACE_COUNTER_CLOCKWISE treats as front-facing, and it
    //     is also the winding loaded glTF meshes arrive with, so both
    //     sources go through the same rasterizer state.
    //   - No triangle is degenerate (zero area).
    //   Both properties are verified on every generated mesh; a violation
    //   throws std::logic_error, so a broken generator is reported at load
    //   time instead of showing up as a missing or inside-out object.
    CoreTypes::MeshData Build(const Primitive_Desc& _desc);

    // =========================================================
    // Individual generators (also callable directly)
    // =========================================================
    // All primitives are UNIT-sized and centered as noted.

    // Hardcoded geometry, no parameters.
    CoreTypes::MeshData Build_cube();        // spans -0.5..0.5, centered
    CoreTypes::MeshData Build_quad();        // 1x1 in XZ plane, centered, facing +Y
    CoreTypes::MeshData Build_triangle();    // unit triangle in XZ, centered, facing +Y
    CoreTypes::MeshData Build_tetrahedron(); // regular tetrahedron, edge 1, centered

    // Parametrized geometry.
    CoreTypes::MeshData Build_plane(uint16_t _subdivisions);               // 1x1 grid in XZ, facing +Y
    CoreTypes::MeshData Build_sphere(uint16_t _segments, uint16_t _rings); // radius 1, centered, one vertex per pole
    CoreTypes::MeshData Build_cone(uint16_t _segments);                    // radius 1 base at y=0, apex at y=1
    CoreTypes::MeshData Build_cylinder(uint16_t _segments);                // radius 1, y from 0 to 1
    CoreTypes::MeshData Build_torus(uint16_t _segments, uint16_t _rings);  // outer radius 1, tube radius 0.25
    CoreTypes::MeshData Build_capsule(uint16_t _segments, uint16_t _rings); // radius 0.25, total height 1 (y from -0.5 to 0.5)

} // namespace ResourceManager::Primitive_Builder
