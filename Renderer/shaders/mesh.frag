#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

// Opaque pass (subpass 0 of the render pass): writes the lit color of the
// surface. The shading itself lives in common/shading.glsl, shared with the
// transparent pass (mesh_oit.frag).
//
// The alpha of the base color is used according to the material's alpha mode
// (Material::alpha_mode), never guessed from its value:
//   OPAQUE - ignored, the surface is solid whatever the alpha of the vertex
//            color, the tint or the texture;
//   MASK   - the fragments whose alpha is below Material::alpha_cutoff are
//            discarded (cut-outs: foliage, fences), the rest are solid.
// BLEND materials are drawn by the transparent pass and never reach this
// shader.
//
// A shader with discard cannot write depth before it knows the fragment
// survives, so on some hardware the early depth write of this pipeline is
// deferred. It is the usual price of alpha testing; a pipeline of its own
// for the MASK materials would avoid it for the OPAQUE ones.

#include "common/frame_set.glsl"
#include "common/scene_data.glsl"
#include "common/bindless.glsl"
#include "common/cluster_data.glsl"
#include "common/safe_math.glsl"
#include "common/shading.glsl"

// Inputs from mesh.vert
#include "common/mesh_varyings.glsl"

layout(location = 0) out vec4 out_color;

void main()
{
    const Material material = material_buffer.materials[frag_material_index];
    const vec4     base     = Material_base_color(material, frag_color, frag_uv);

    if (material.alpha_mode == ALPHA_MODE_MASK && base.a < material.alpha_cutoff)
        discard;

    const float view_depth = View_depth(frag_world_pos);
    const vec3  color      = Shade_surface(base.rgb, frag_world_pos, frag_world_normal, gl_FragCoord.xy, view_depth);

    // The color attachment is written without blending: the alpha is
    // explicitly solid, not whatever the product of the three alphas is.
    out_color = vec4(color, 1.0);
}
