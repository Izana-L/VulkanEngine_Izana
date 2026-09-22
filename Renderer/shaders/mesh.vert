#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/frame_set.glsl"
#include "common/push_constants.glsl"

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

void main()
{
    vec4 world_pos = push.model * vec4(in_position, 1.0);

    gl_Position = frame.view_projection * world_pos;

    // Transform normal to world space (handles non-uniform scale).
    mat3 normal_matrix = transpose(inverse(mat3(push.model)));
    frag_world_normal  = normalize(normal_matrix * in_normal);

    frag_world_pos = world_pos.xyz;
    frag_uv        = in_uv;
    frag_color     = in_color;
}
