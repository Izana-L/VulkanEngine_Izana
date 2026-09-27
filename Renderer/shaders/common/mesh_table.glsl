#ifndef MESH_TABLE_GLSL
#define MESH_TABLE_GLSL

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

layout(set = 2, binding = 1, std430) readonly buffer Mesh_Table_Buffer
{
    Mesh_Info meshes[];
} mesh_table;

// Largest scale the upper 3x3 of _model applies along any axis: the
// length of its longest column. Multiplying a radius by it keeps the
// sphere enclosing under non-uniform scale; the mean or a single column
// would let stretched objects poke out of their sphere.
float Max_axis_scale(mat4 _model)
{
    const float x = dot(_model[0].xyz, _model[0].xyz);
    const float y = dot(_model[1].xyz, _model[1].xyz);
    const float z = dot(_model[2].xyz, _model[2].xyz);
    return sqrt(max(max(x, y), z));
}

// Bounding sphere of a mesh placed by _model: world space center in xyz,
// radius in w.
vec4 World_bounding_sphere(mat4 _model, vec4 _local_sphere)
{
    const vec3 center = (_model * vec4(_local_sphere.xyz, 1.0)).xyz;
    return vec4(center, _local_sphere.w * Max_axis_scale(_model));
}

#endif
