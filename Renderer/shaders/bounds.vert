#version 450
#extension GL_GOOGLE_include_directive : require

// Bounding volume debug view: draws the volume the culling tests for every
// object of the frame, so it can be checked that each one encloses its
// mesh.
//
// One instanced draw of a unit sphere mesh (radius 1, centered at the
// origin) with firstInstance 0 and one instance per object entry:
// gl_InstanceIndex is then the object index. Each vertex is mapped onto
// the mesh's local bounding sphere (center + radius * unit vertex) and
// transformed by the model matrix: the result is the bounding ellipsoid
// the culling tests plane by plane (mesh_table.glsl), under non-uniform
// scale and shear included. What is drawn is exactly what is tested, and
// it encloses the mesh by construction, since the local sphere encloses
// every vertex and both go through the same matrix.

#include "common/frame_set.glsl"
#include "common/scene_data.glsl"
#include "common/mesh_table.glsl"

// Vertex inputs - matches CoreTypes::Vertex_Static_Mesh, the layout every
// graphics pipeline declares. Only the position is read; the other four
// are declared so that every attribute of the pipeline is consumed by the
// shader interface, which keeps the validation layer from reporting
// attributes that are not consumed.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_color;

// flat: the flags of the object reach every fragment unchanged.
layout(location = 0) flat out uint frag_flags;

void main()
{
    const Object    object = object_buffer.objects[gl_InstanceIndex];
    const Mesh_Info mesh   = mesh_table.meshes[object.mesh_index];

    // Point of the local bounding sphere, mesh space.
    const vec3 local_position = mesh.bounding_sphere.xyz + in_position * mesh.bounding_sphere.w;

    gl_Position = frame.view_projection * (object.model * vec4(local_position, 1.0));
    frag_flags  = object.flags;
}
