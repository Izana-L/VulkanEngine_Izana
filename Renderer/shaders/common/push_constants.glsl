#ifndef PUSH_CONSTANTS_GLSL
#define PUSH_CONSTANTS_GLSL

// EXACT mirror of Renderer_System::Push_Constants (Frame_Data.hpp).
// One block shared by both stages: the vertex shader reads `model`, the
// fragment shader reads `base_color` and `albedo_texture_index`. The
// pipeline layout declares a single 96-byte range for VERTEX | FRAGMENT.
layout(push_constant) uniform Push_Constants
{
    mat4 model;                 // offset 0
    vec4 base_color;            // offset 64: per-draw tint, alpha < 1 = transparent pass
    uint albedo_texture_index;  // offset 80: bindless index, or INVALID_TEXTURE_INDEX
} push;

// Mirror of CoreTypes::INVALID_TEXTURE_INDEX.
const uint INVALID_TEXTURE_INDEX = 0xFFFFFFFFu;

#endif
