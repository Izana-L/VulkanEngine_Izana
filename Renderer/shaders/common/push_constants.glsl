#ifndef PUSH_CONSTANTS_GLSL
#define PUSH_CONSTANTS_GLSL

// EXACT mirror of Renderer_System::Push_Constants (Frame_Data.hpp).
// One block shared by both stages: the vertex shader reads `model`, the
// fragment shader reads `base_color`, `albedo_texture_index` and
// `albedo_sampler_index`. The pipeline layout declares a single 96-byte
// range for VERTEX | FRAGMENT; the two trailing padding words of the C++
// struct are not declared here because nothing reads them.
layout(push_constant) uniform Push_Constants
{
    mat4 model;                 // offset 0
    vec4 base_color;            // offset 64: per-draw tint, alpha < 1 = transparent pass
    uint albedo_texture_index;  // offset 80: bindless index, or INVALID_TEXTURE_INDEX
    uint albedo_sampler_index;  // offset 84: slot in samplers[] (a CoreTypes::Sampler_Preset value)
} push;

// Mirror of CoreTypes::INVALID_TEXTURE_INDEX.
const uint INVALID_TEXTURE_INDEX = 0xFFFFFFFFu;

#endif
