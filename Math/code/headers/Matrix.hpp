#pragma once

// glm configuration used by every module. Defined here, in the headers
// every engine translation unit includes before any other glm header, so
// the clip-space convention and the availability of the GTX extensions do
// not depend on which project property sheet a module happens to import.
//   GLM_FORCE_DEPTH_ZERO_TO_ONE: Vulkan's [0, 1] depth range.
//   GLM_ENABLE_EXPERIMENTAL:     the GTX headers the Math module wraps.
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
namespace MathLib
{
    template< unsigned COLUMNS, unsigned ROWS, typename TYPE >
    using Matrix = glm::mat< static_cast<glm::length_t>(COLUMNS), static_cast<glm::length_t>(ROWS), TYPE >;

    using Matrix2 = Matrix< 2, 2, float >;
    using Matrix3 = Matrix< 3, 3, float >;
    using Matrix4 = Matrix< 4, 4, float >;

}