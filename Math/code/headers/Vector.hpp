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

    template< unsigned DIMENSION, typename TYPE >
    using Vector = glm::vec< DIMENSION, TYPE >;

    using Vector2 = Vector< 2, float >;
    using Vector2i = Vector< 2, int >;
    using Vector2u = Vector< 2, unsigned >;

    using Vector3 = Vector< 3, float >;
    using Vector3i = Vector< 3, int >;
    using Vector3u = Vector< 3, unsigned >;

    using Vector4 = Vector< 4, float >;
    using Vector4i = Vector< 4, int >;
    using Vector4u = Vector< 4, unsigned >;

}