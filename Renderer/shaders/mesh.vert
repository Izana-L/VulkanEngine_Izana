#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/frame_set.glsl"
#include "common/scene_data.glsl"

// Vertex inputs - matches CoreTypes::Vertex_Static_Mesh
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_color;

// Outputs to fragment shader
layout(location = 0) out vec3 frag_world_normal;
layout(location = 1) out vec3 frag_world_pos;
layout(location = 2) out vec2 frag_uv;
layout(location = 3) out vec4 frag_color;
// flat: an index must reach every fragment unchanged. Without it the value
// is interpolated between the three vertices, with no compilation or
// validation error, only wrong materials.
layout(location = 4) flat out uint frag_material_index;

void main()
{
    // The draw's firstInstance is the index of its entry in the object
    // buffer, and gl_InstanceIndex already includes it.
    Object object = object_buffer.objects[gl_InstanceIndex];

    vec4 world_pos = object.model * vec4(in_position, 1.0);

    gl_Position = frame.view_projection * world_pos;

    // Normal to world space with the inverse-transpose computed on the CPU
    // (handles non-uniform scale); no per-vertex inverse().
    frag_world_normal = normalize(mat3(object.normal_matrix) * in_normal);

    frag_world_pos      = world_pos.xyz;
    frag_uv             = in_uv;
    frag_color          = in_color;
    frag_material_index = object.material_index;
}