#ifndef MESH_VARYINGS_GLSL
#define MESH_VARYINGS_GLSL

// Set and binding numbers, locations and flag bits shared with C++.
#include "gpu_shared.h"

// The interface between mesh.vert and the fragment shaders that shade a
// mesh (mesh.frag, mesh_oit.frag), declared once for both ends: mesh.vert
// defines MESH_VARYINGS_OUTPUT before including this file, the fragment
// shaders do not. A varying that differs between the two ends (location,
// type, or the flat qualifier) compiles without any error and only shows
// as wrong values, so there is no second copy to keep in step.
#ifdef MESH_VARYINGS_OUTPUT
#define MESH_VARYING out
#else
#define MESH_VARYING in
#endif

layout(location = GPU_VARYING_WORLD_NORMAL)   MESH_VARYING vec3 frag_world_normal;
layout(location = GPU_VARYING_WORLD_POSITION) MESH_VARYING vec3 frag_world_pos;
layout(location = GPU_VARYING_UV)             MESH_VARYING vec2 frag_uv;
layout(location = GPU_VARYING_COLOR)          MESH_VARYING vec4 frag_color;

// flat: an index must reach every fragment unchanged. Without it the value
// is interpolated between the three vertices, with no compilation or
// validation error, only wrong materials.
layout(location = GPU_VARYING_MATERIAL_INDEX) flat MESH_VARYING uint frag_material_index;

#endif
