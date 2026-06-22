#pragma once
#include <glm/glm.hpp>
#include <Math_Types.hpp>
namespace MathLib 
{
    template< unsigned COLUMNS, unsigned ROWS, typename TYPE >
    using Matrix = glm::mat< static_cast<glm::length_t>(COLUMNS), static_cast<glm::length_t>(ROWS), TYPE >;

    using Matrix2 = Matrix< 2, 2, float >;
    using Matrix3 = Matrix< 3, 3, float >;
    using Matrix4 = Matrix< 4, 4, float >;

}