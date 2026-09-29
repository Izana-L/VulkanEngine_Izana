#pragma once

#include <MeshData.hpp>
#include <Primitive_Desc.hpp>
#include <Vector.hpp>

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
    //   - UV origin at the TOP-LEFT of the image, as in glTF and Vulkan
    //     (stb_image returns the top row first and Vulkan samples v = 0 on
    //     the first row in memory, so no flip happens anywhere). Seen from
    //     outside in its canonical orientation, every face has u growing
    //     to the right and v growing downwards. Canonical orientation:
    //       side faces           image up = +Y;
    //       faces towards +Y     seen from above, image up = -Z;
    //       faces towards -Y     seen from below, image up = +Z.
    //     The tangent's w follows the glTF convention (the bitangent
    //     cross(N, T) * w points up in the image), so every primitive has
    //     w = +1.
    //   The winding, the non-zero area and the absence of a mirrored UV
    //   mapping are verified on every generated mesh; a violation throws
    //   std::logic_error, so a broken generator is reported at load time
    //   instead of showing up as a missing, inside-out or mirrored object.
    //   (A mapping rotated 180 degrees is not a mirror and is not caught
    //   by that check; the orientation rules above define it.)
    //
    // Surfaces of revolution (sphere, capsule, cylinder, cone, torus) are
    // parametrized around +Y by the same azimuth:
    //     theta = PI/2 + 2*PI*u,   radial direction = (cos theta, 0, -sin theta)
    // theta grows counter-clockwise seen from +Y, so u grows to the right
    // seen from outside. The seam (u = 0 = 1) lies on -Z, behind the object
    // as seen by the default camera (looking down -Z), and u = 0.5 faces
    // +Z. v runs from the top (v = 0) to the bottom (v = 1); on the torus
    // it starts at the outer equator and goes down the outer face first.
    CoreTypes::MeshData Build(const Primitive_Desc& _desc);

    // Texture coordinate of the unit sphere (Build_sphere) in the direction
    // _direction from its center (normalized here):
    //     u = fract((atan2(-d.z, d.x) - PI/2) / (2*PI)),   v = acos(d.y) / PI
    // The inverse of the sphere's parametrization. Any equirectangular
    // lookup (skybox, image based lighting) must use this same mapping,
    // in the shaders as well, or the environment and a sphere textured
    // with the same image end up rotated or mirrored against each other.
    MathLib::Vector2 Sphere_uv_of_direction(const MathLib::Vector3& _direction);

    // =========================================================
    // Individual generators (also callable directly)
    // =========================================================
    // All primitives are UNIT-sized and centered as noted.

    // Hardcoded geometry, no parameters.
    CoreTypes::MeshData Build_cube();        // spans -0.5..0.5, centered
    CoreTypes::MeshData Build_quad();        // alias of Build_plane(1): 1x1 in XZ plane, centered, facing +Y
    CoreTypes::MeshData Build_triangle();    // unit triangle in XZ, centered, facing +Y
    CoreTypes::MeshData Build_tetrahedron(); // regular tetrahedron, edge 1, centered

    // Parametrized geometry.
    CoreTypes::MeshData Build_plane(uint16_t _subdivisions);               // 1x1 grid in XZ, facing +Y
    CoreTypes::MeshData Build_sphere(uint16_t _segments, uint16_t _rings); // radius 1, centered, one pole vertex per segment
    CoreTypes::MeshData Build_cone(uint16_t _segments);                    // radius 1 base at y=0, apex at y=1
    CoreTypes::MeshData Build_cylinder(uint16_t _segments);                // radius 1, y from 0 to 1
    CoreTypes::MeshData Build_torus(uint16_t _segments, uint16_t _rings);  // outer radius 1, tube radius 0.25
    CoreTypes::MeshData Build_capsule(uint16_t _segments, uint16_t _rings); // radius 0.25, total height 1 (y from -0.5 to 0.5)

} // namespace ResourceManager::Primitive_Builder
