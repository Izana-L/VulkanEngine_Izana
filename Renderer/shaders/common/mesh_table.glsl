#ifndef MESH_TABLE_GLSL
#define MESH_TABLE_GLSL

// Set and binding numbers shared with C++.
#include "gpu_shared.h"

// Bounding_ellipsoid_center, Bounding_ellipsoid_extent, Ellipsoid_in_frustum.
#include "bounding_math.glsl"

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

// The bounding ellipsoid of a mesh under a model matrix, and the frustum
// test the culling applies to it, are in bounding_math.glsl, a set of pure
// functions shared with the shader self-test.

#endif
