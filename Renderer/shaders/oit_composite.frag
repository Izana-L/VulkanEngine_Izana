#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/gpu_shared.h"
#include "common/safe_math.glsl"

// Composite of the weighted blended OIT (subpass 2 of the render pass).
//
// Reads, at the pixel being shaded, the two targets the transparent
// subpass accumulated (mesh_oit.frag) and outputs the weighted average
// color of the transparent layers with alpha = 1 - revealage. The pipeline
// blends it "over" the opaque color (SRC_ALPHA, ONE_MINUS_SRC_ALPHA):
//   final = average * (1 - revealage) + opaque * revealage
// where revealage, the product of (1 - alpha) of every layer, is the
// fraction of the opaque color that stays visible.

// Set 1 of the graphics pipeline layout (Binding_Graphics_Pass). The
// input_attachment_index is the position in the pInputAttachments of the
// composite subpass (Vulkan_Render_Pass.cpp), from the same macros.
layout(input_attachment_index = GPU_OIT_INPUT_ACCUMULATION, set = GPU_SET_PER_PASS, binding = GPU_BINDING_OIT_ACCUMULATION) uniform subpassInput oit_accumulation;
layout(input_attachment_index = GPU_OIT_INPUT_REVEALAGE, set = GPU_SET_PER_PASS, binding = GPU_BINDING_OIT_REVEALAGE) uniform subpassInput oit_revealage;

layout(location = 0) out vec4 out_color;

// Smallest accumulated weight divided by: keeps the average finite for
// layers whose alpha * weight rounds to zero in half precision.
const float MIN_ACCUMULATED_WEIGHT = 1.0e-5;

void main()
{
    const float revealage = subpassLoad(oit_revealage).r;

    // No transparent layer covers the pixel: the target still holds its
    // clear value and the opaque color stays untouched, without blending.
    if (revealage >= 1.0)
        discard;

    vec4 accumulation = subpassLoad(oit_accumulation);

    // A sum beyond the half-precision range (more layers at the maximum
    // weight than mesh_oit.frag bounds for) has lost its average, and a
    // NaN in the sum (mesh_oit.frag drops the fragments that would add
    // one, so this does not come from the shading) has spoiled it for good:
    // in both cases the layers are shown white instead of propagating
    // infinities or NaN into the swapchain image.
    if (!Is_finite(accumulation))
        accumulation = vec4(1.0);

    const vec3 average = accumulation.rgb / max(accumulation.a, MIN_ACCUMULATED_WEIGHT);

    out_color = vec4(average, 1.0 - revealage);
}
