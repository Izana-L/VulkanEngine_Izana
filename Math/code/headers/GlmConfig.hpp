#pragma once

// Single entry point to glm for the whole engine. Every Math header
// includes glm through this file, and no module outside Math includes glm
// directly: they use the MathLib headers.
//
// The same macros are also defined by Game/projects/GlmConfig.props, which
// every project imports. The copy here keeps the configuration correct for
// a translation unit built without that property sheet.
//   GLM_FORCE_DEPTH_ZERO_TO_ONE: Vulkan's [0, 1] depth range.
//   GLM_ENABLE_EXPERIMENTAL:     the GTX headers the Math module wraps.
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>

// glm fixes its clip space convention the first time it is included. If
// glm reached this translation unit before this header, without the
// property sheet, the defines above came too late and glm::perspective
// would silently map depth to [-1, 1].
static_assert(GLM_CONFIG_CLIP_CONTROL& GLM_CLIP_CONTROL_ZO_BIT,
    "glm was configured without GLM_FORCE_DEPTH_ZERO_TO_ONE: include GlmConfig.hpp before any glm header");