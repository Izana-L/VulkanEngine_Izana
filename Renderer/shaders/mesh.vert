#version 450

// Uniform buffer  set 0, binding 0
// Matches Frame_UBO in Frame_Data.hpp: { mat4 view; mat4 projection; }
layout(set = 0, binding = 0) uniform Frame_UBO
{
    mat4 view;
    mat4 projection;
} ubo;

// Model matrix as push constant  one per draw call.
layout(push_constant) uniform Push_Constants
{
    mat4 model;
} push;

// Vertex inputs  matches CoreTypes::Vertex_Static_Mesh
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

    gl_Position = ubo.projection * ubo.view * world_pos;

    // Transform normal to world space (handles non-uniform scale).
    mat3 normal_matrix = transpose(inverse(mat3(push.model)));
    frag_world_normal  = normalize(normal_matrix * in_normal);

    frag_world_pos = world_pos.xyz;
    frag_uv        = in_uv;
    frag_color     = in_color;
}
