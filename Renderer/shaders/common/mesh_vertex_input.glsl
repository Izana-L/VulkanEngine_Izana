#ifndef MESH_VERTEX_INPUT_GLSL
#define MESH_VERTEX_INPUT_GLSL

// Set and binding numbers, locations and flag bits shared with C++.
#include "gpu_shared.h"

// Vertex inputs of every pipeline that draws a lit mesh: matches
// CoreTypes::Vertex_Static_Mesh and the attribute descriptions of
// Vulkan_Vertex_Layout.hpp, which take their locations from the same
// GPU_VERTEX_LOCATION_* macros. All five are declared by every shader that
// includes this file, whether it reads them or not, so each attribute of
// the pipeline is consumed by the shader interface (the validation layer
// reports attributes that are not).
layout(location = GPU_VERTEX_LOCATION_POSITION) in vec3 in_position;
layout(location = GPU_VERTEX_LOCATION_NORMAL)   in vec3 in_normal;
layout(location = GPU_VERTEX_LOCATION_TANGENT)  in vec4 in_tangent;
layout(location = GPU_VERTEX_LOCATION_UV)       in vec2 in_uv;
layout(location = GPU_VERTEX_LOCATION_COLOR)    in vec4 in_color;

#endif
