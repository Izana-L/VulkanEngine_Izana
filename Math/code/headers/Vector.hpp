#pragma once
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