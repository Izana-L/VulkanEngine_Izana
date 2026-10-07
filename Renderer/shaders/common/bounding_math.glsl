#ifndef BOUNDING_MATH_GLSL
#define BOUNDING_MATH_GLSL

// Set and binding numbers, array sizes and enum values shared with C++.
#include "gpu_shared.h"

// ── Bounding ellipsoid ────────────────────────────────────────
// The bounding sphere of a mesh (Mesh_Info::bounding_sphere, mesh space)
// placed by a model matrix is an ellipsoid. The culling tests it directly,
// plane by plane, instead of enclosing it in a world space sphere: a
// sphere scaled by the longest column of the 3x3 only bounds T * R * S
// matrices, and a rotated child under a non-uniformly scaled parent has
// shear (Transform_System composes world = parent world * local), which
// can make that radius up to 1 / sqrt(3) of the real extent.
//
// With M the upper 3x3 of the model (columns M0, M1, M2), (c, rho) the
// local sphere and (n, w) a plane with its normal pointing inside:
//   d = dot(n, M * c + t) + w     signed distance of the center,
//   r = rho * |M^T n|             half-width of the ellipsoid along n,
//                                 M^T n = (M0.n, M1.n, M2.n),
// and the ellipsoid lies entirely outside when d < -r. Exact for any
// affine matrix (non-uniform scale, shear, reflections), tighter than any
// world sphere around the ellipsoid, and independent of the normal's
// length, since d and r scale alike.
//
// Pure functions of explicit arguments, with no descriptor: the culling
// pass calls them with the planes of the frame, and selftest.comp calls the
// very same code with test values, to compare it with its C++ twin,
// Frustum::Intersects_ellipsoid (RenderPacket.hpp), which culls
// the transparent items on the CPU: both evaluate the same formula.

// World space center of the bounding ellipsoid.
vec3 Bounding_ellipsoid_center(mat4 _model, vec4 _local_sphere)
{
    return (_model * vec4(_local_sphere.xyz, 1.0)).xyz;
}

// Half-width of the bounding ellipsoid measured along _normal:
// _local_radius * |M^T _normal|.
float Bounding_ellipsoid_extent(mat4 _model, float _local_radius, vec3 _normal)
{
    const vec3 transposed_normal = vec3(dot(_model[0].xyz, _normal),
                                        dot(_model[1].xyz, _normal),
                                        dot(_model[2].xyz, _normal));

    return _local_radius * length(transposed_normal);
}

// False when the bounding ellipsoid of the mesh, placed by _model, lies
// entirely on the outer side of one plane: signed distance of its center
// below minus its half-width along the plane normal. A volume crossing a
// plane, the near one included, is kept: it is partly visible. _planes are
// world space planes with their normals pointing inside (xyz = normal,
// w = offset).
bool Ellipsoid_in_frustum(mat4 _model, vec4 _local_sphere, vec4 _planes[GPU_FRUSTUM_PLANE_COUNT])
{
    const vec3 center = Bounding_ellipsoid_center(_model, _local_sphere);

    for (int p = 0; p < GPU_FRUSTUM_PLANE_COUNT; ++p)
    {
        const vec4 plane = _planes[p];

        if (dot(plane.xyz, center) + plane.w < -Bounding_ellipsoid_extent(_model, _local_sphere.w, plane.xyz))
            return false;
    }

    return true;
}

#endif
