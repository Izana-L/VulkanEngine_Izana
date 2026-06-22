#version 450

layout(binding = 0) uniform MVP_UBO {
    mat4 model;
    mat4 view;
    mat4 projection;
} mvp;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_color;

layout(location = 0) out vec4 frag_color;

void main() {
    gl_Position = mvp.projection * mvp.view * mvp.model * vec4(in_position, 1.0);
    frag_color = in_color;
}