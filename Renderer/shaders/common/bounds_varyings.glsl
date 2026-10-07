#ifndef BOUNDS_VARYINGS_GLSL
#define BOUNDS_VARYINGS_GLSL

// Set and binding numbers, locations and flag bits shared with C++.
#include "gpu_shared.h"

// The interface between bounds.vert and bounds.frag, declared once for both
// ends: bounds.vert defines BOUNDS_VARYINGS_OUTPUT before including this
// file, bounds.frag does not (see mesh_varyings.glsl).
#ifdef BOUNDS_VARYINGS_OUTPUT
#define BOUNDS_VARYING out
#else
#define BOUNDS_VARYING in
#endif

// flat: the flags of the object reach every fragment unchanged.
layout(location = GPU_VARYING_OBJECT_FLAGS) flat BOUNDS_VARYING uint frag_flags;

#endif
