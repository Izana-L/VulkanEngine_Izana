#version 450
#extension GL_GOOGLE_include_directive : require

// Bounding sphere debug view (see bounds.vert). Green for opaque objects,
// blue for transparent ones. The alpha is used only by the fallback
// pipeline that draws filled, blended spheres when the device lacks
// fillModeNonSolid; the wireframe pipeline does not blend.

#include "common/scene_data.glsl"

layout(location = 0) flat in uint frag_flags;

layout(location = 0) out vec4 out_color;

void main()
{
    const bool transparent = (frag_flags & RENDER_PASS_TRANSPARENT) != 0u;

    out_color = transparent ? vec4(0.25, 0.55, 1.0, 0.25) : vec4(0.3, 1.0, 0.35, 0.25);
}
