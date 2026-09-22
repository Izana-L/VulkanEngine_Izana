#version 450
#extension GL_GOOGLE_include_directive : require

// Shares the frame block and the push constant block with mesh.vert, so
// it cannot drift from the layouts the Renderer actually uploads.
#include "common/frame_set.glsl"
#include "common/push_constants.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_color;

void main()
{
    gl_Position = frame.projection * frame.view * push.model * vec4(in_position, 1.0);
}
