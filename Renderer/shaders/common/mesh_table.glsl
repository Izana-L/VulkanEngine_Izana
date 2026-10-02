#ifndef MESH_TABLE_GLSL
#define MESH_TABLE_GLSL

// Set and binding numbers shared with C++.
#include "gpu_shared.h"

// Mesh table: set 2, binding 1 (Binding_Per_Material::Meshes). One entry
// per mesh gpu id, written when the mesh is uploaded, indexed by
// Object::mesh_index. Visible to the vertex and compute stages only.
//
// Every mesh lives in the shared geometry pool: first_index and
// vertex_offset are the firstIndex / vertexOffset of its draw, counted in
// indices and vertices.

// EXACT mirror of Renderer_System::Mesh_Info_GPU, std430, 32 bytes.
struct Mesh_Info
{
    vec4 bounding_sphere;       // offset 0:  xyz = center, w = radius, mesh space
    uint first_index;           // offset 16
    uint index_count;           // offset 20
    int  vertex_offset;         // offset 24
    uint vertex_count;          // offset 28
};

layout(set = GPU_SET_PER_MATERIAL, binding = GPU_BINDING_MESHES, std430) readonly buffer Mesh_Table_Buffer
{
    Mesh_Info meshes[];
} mesh_table;

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
// Mirrored by CoreTypes::Frustum::Intersects_ellipsoid (RenderPacket.hpp)
// for the CPU culling of the transparent items: both evaluate the same
// formula.

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

#endif
