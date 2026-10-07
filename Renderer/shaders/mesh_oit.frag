#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

// Transparent pass (subpass 1 of the render pass): weighted blended
// order-independent transparency (McGuire and Bavoil, JCGT 2013).
//
// Every transparent fragment is shaded exactly like an opaque one
// (common/shading.glsl) and, instead of being blended over the color
// buffer, is accumulated into two targets (Vulkan_OIT_Resources):
//   location 0, accumulation (RGBA16F, cleared to 0, blending ONE, ONE):
//       += (color * alpha * w, alpha * w)
//   location 1, revealage (R16F, cleared to 1, blending ZERO,
//       ONE_MINUS_SRC_COLOR):
//       *= (1 - alpha)
// The composite subpass (oit_composite.frag) then blends the weighted
// average color accumulation.rgb / accumulation.a over the opaque color
// with opacity 1 - revealage.
//
// Neither sum depends on the order the fragments arrive in, so the draws
// need no sorting and objects that intersect, contain one another or
// overlap cyclically are handled like any other. What is exact and what is
// not:
//   - the fraction of background left visible, the product of
//     (1 - alpha), is exact;
//   - the color is a weighted average of the layers: exact when every
//     layer has the same color, an approximation otherwise (with high
//     alpha and different colors, the front layer does not fully hide the
//     one behind it). w favours the layers closer to the camera.
// Only "over" blending (and additive, with alpha 0) is covered: a
// multiplicative (colored filter) transparency needs an extension of the
// method. Depth is tested against the opaque surfaces and never written.

#include "common/frame_set.glsl"
#include "common/scene_data.glsl"
#include "common/bindless.glsl"
#include "common/cluster_data.glsl"
#include "common/safe_math.glsl"
#include "common/shading.glsl"

// Inputs from mesh.vert
#include "common/mesh_varyings.glsl"

// Outputs: the position in the pColorAttachments of the subpass
// (Vulkan_Render_Pass.cpp), from the same macros.
layout(location = GPU_OIT_OUTPUT_ACCUMULATION) out vec4  out_accumulation;
layout(location = GPU_OIT_OUTPUT_REVEALAGE)    out float out_revealage;

// ── Weight function ───────────────────────────────────────────
// Equation 7 of McGuire and Bavoil:
//   w(z, alpha) = alpha * clamp(10 / (1e-5 + (z / 5)^2 + (z / 200)^6), OIT_WEIGHT_MIN, OIT_WEIGHT_MAX)
// with z the linear view depth of the fragment (View_depth). gl_FragCoord.z
// cannot be used: under the infinite reverse-Z projection it holds
// near / distance.
//
// Its depth constants suit the range of the engine's scenes: the camera
// near plane defaults to 1 (MathLib::Constants::NEAR_PLANE_DEFAULT) and the
// clustered lighting ends at 200 (CLUSTER_MAX_DISTANCE). Over that range
// the quadratic term dominates: the weight goes from 250 at depth 1 to
// 2.5 at depth 10 and reaches the floor near depth 160; the sixth power
// term flattens everything beyond 200. A scene with a very different depth
// range needs other constants: with too flat a weight the layers mix
// regardless of depth, with too steep a one the fp16 sums lose the far
// layers.
const float OIT_WEIGHT_NEAR_SCALE = 5.0;
const float OIT_WEIGHT_FAR_SCALE  = 200.0;

// Bounds of the depth weight. The floor keeps distant layers from
// vanishing entirely. The ceiling bounds the accumulation: each layer adds
// at most OIT_WEIGHT_MAX to every channel (color is clamped to 1, alpha is
// at most 1), so up to 65 layers fit under 65504, the largest finite
// half-precision value. With the near plane at 1 the weight never goes
// above 250 (262 layers); the ceiling only matters for closer near planes.
const float OIT_WEIGHT_MIN = 1.0e-2;
const float OIT_WEIGHT_MAX = 1.0e3;

float Oit_weight(float _view_depth, float _alpha)
{
    const float near_term = _view_depth / OIT_WEIGHT_NEAR_SCALE;
    const float far_term  = _view_depth / OIT_WEIGHT_FAR_SCALE;
    const float far_term3 = far_term * far_term * far_term;

    const float depth_weight = 10.0 / (1.0e-5 + near_term * near_term + far_term3 * far_term3);

    return _alpha * clamp(depth_weight, OIT_WEIGHT_MIN, OIT_WEIGHT_MAX);
}

void main()
{
    const Material material = material_buffer.materials[frag_material_index];
    const vec4     base     = Material_base_color(material, frag_color, frag_uv);
    const float    view_depth = View_depth(frag_world_pos);
    const float    alpha      = clamp(base.a, 0.0, 1.0);

    const vec3 lit = Shade_surface(base.rgb, frag_world_pos, frag_world_normal, gl_FragCoord.xy, view_depth);

    // The accumulation is a sum: a NaN or an infinity added to it cannot be
    // taken back, and would spoil the pixel for every layer of every object
    // over it, not only for this fragment. A fragment with a non-finite color
    // is dropped instead (the normals and lights that reach this point are
    // finite by construction; this is the last line of defense).
    if (!Is_finite(lit))
        discard;

    // The swapchain is a UNORM target: the fixed-function blending of the
    // sorted path clamped every source color to [0, 1] before blending it.
    // The same clamp keeps the look of that path and bounds the
    // accumulation (see OIT_WEIGHT_MAX). An HDR target would replace it
    // with a larger bound.
    const vec3 color = clamp(lit, 0.0, 1.0);

    const float weight = Oit_weight(view_depth, alpha);

    out_accumulation = vec4(color * alpha, alpha) * weight;
    out_revealage    = alpha;
}
