#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace MathLib 
{

    // =========================================================
    // Vector types
    // =========================================================

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

    // =========================================================
    // Matrix types
    // =========================================================

    template< unsigned COLUMNS, unsigned ROWS, typename TYPE >
    using Matrix = glm::mat< static_cast<glm::length_t>(COLUMNS), static_cast<glm::length_t>(ROWS), TYPE >;

    using Matrix2 = Matrix< 2, 2, float >;
    using Matrix3 = Matrix< 3, 3, float >;
    using Matrix4 = Matrix< 4, 4, float >;

    // =========================================================
    // Quaternion
    // =========================================================

    using Quaternion = glm::quat;

}