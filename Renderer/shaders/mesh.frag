#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

// Opaque pass (subpass 0 of the render pass): writes the lit color of the
// surface. The shading itself lives in common/shading.glsl, shared with the
// transparent pass (mesh_oit.frag).

#include "common/frame_set.glsl"
#include "common/scene_data.glsl"
#include "common/bindless.glsl"
#include "common/cluster_data.glsl"
#include "common/shading.glsl"

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec3 frag_world_pos;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in vec4 frag_color;
layout(location = 4) flat in uint frag_material_index;

layout(location = 0) out vec4 out_color;

void main()
{
    const vec4  base       = Material_base_color(frag_material_index, frag_color, frag_uv);
    const float view_depth = View_depth(frag_world_pos);
    const vec3  color      = Shade_surface(base.rgb, frag_world_pos, frag_world_normal, gl_FragCoord.xy, view_depth);

    out_color = vec4(color, base.a);
}
