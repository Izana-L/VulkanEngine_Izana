#version 450

layout(set = 0, binding = 0) uniform Frame_UBO
{
    mat4 view;
    mat4 projection;
} ubo;

layout(push_constant) uniform Push_Constants
{
    mat4 model;
} push;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_color;

void main()
{
    gl_Position = ubo.projection * ubo.view * push.model * vec4(in_position, 1.0);
}
