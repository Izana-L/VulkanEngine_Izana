#ifndef FRAME_SET_GLSL
#define FRAME_SET_GLSL

// EXACT mirror of Renderer_System::Frame_UBO (Frame_Data.hpp), std140.
// A field added here and not there (or the other way round) shifts every
// following offset without any warning. Both are edited in the same
// commit, always, and the C++ side pins the offsets with static_asserts.
//
// Only fields with a consumer are here:
//   view / projection  - debug.vert
//   view_projection    - mesh.vert
//   camera_position    - view-dependent lighting terms (mesh.frag)
//   light_count        - mesh.frag
layout(set = 0, binding = 0, std140) uniform Frame_UBO
{
    mat4  view;
    mat4  projection;
    mat4  view_projection;
    vec3  camera_position;
    int   light_count;
} frame;

// EXACT mirror of Renderer_System::Light_GPU, std430.
struct Light
{
    vec3  position_or_direction;
    float intensity;
    vec3  color;
    float range;
    vec3  spot_direction;
    float inner_angle;
    float outer_angle;
    int   type;                 // 0 = directional, 1 = point, 2 = spot
    float _pad0;
    float _pad1;
};

// Unsized array: what an SSBO allows and a UBO does not. Raising the
// buffer capacity (MAX_LIGHTS in Frame_Data.hpp) does not touch this file.
// readonly: the shader never writes, and saying so lets the driver optimize.
layout(set = 0, binding = 1, std430) readonly buffer Light_Buffer
{
    Light lights[];
} light_buffer;

#endif
