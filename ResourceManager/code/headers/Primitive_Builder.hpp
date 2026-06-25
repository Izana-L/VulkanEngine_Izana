#pragma once

#include <MeshData.hpp>
#include <Primitive_Desc.hpp>

namespace ResourceManager::Primitive_Builder
{

    // Builds a unit-sized procedural mesh from a Primitive_Desc.
    // Dispatches on desc.type to the appropriate generator below.
    // All output meshes have positions, normals, UVs and tangents
    // computed, with UINT32 indices.
    CoreTypes::MeshData Build(const Primitive_Desc& _desc);

    // =========================================================
    // Individual generators (also callable directly)
    // =========================================================
    // All primitives are UNIT-sized and centered as noted.

    // Hardcoded geometry — no parameters.
    CoreTypes::MeshData Build_cube();        // spans -0.5..0.5, centered
    CoreTypes::MeshData Build_quad();        // 1x1 in XZ plane, centered, facing +Y
    CoreTypes::MeshData Build_triangle();    // unit triangle in XZ, centered
    CoreTypes::MeshData Build_tetrahedron(); // unit tetrahedron, centered

    // Parametrized geometry.
    CoreTypes::MeshData Build_plane(uint16_t _subdivisions);              // 1x1 grid in XZ
    CoreTypes::MeshData Build_sphere(uint16_t _segments, uint16_t _rings); // radius 1, centered
    CoreTypes::MeshData Build_cone(uint16_t _segments);                  // radius 1, height 1
    CoreTypes::MeshData Build_cylinder(uint16_t _segments);                  // radius 1, height 1
    CoreTypes::MeshData Build_torus(uint16_t _segments, uint16_t _rings); // outer radius 1
    CoreTypes::MeshData Build_capsule(uint16_t _segments, uint16_t _rings); // radius 1, height 1

} // namespace ResourceManager::Primitive_Builder