#pragma once
#include <Matrix.hpp>
#include <MathConstants.hpp>
#include <Vector.hpp>
#include <cmath>

namespace MathLib 
{
    
    namespace Mat2 
    {
        // 2x2 matrix, primarily used for:
        //   - 2D rotations and scales (without translation)
        //   - Linear transformations in 2D space
        //   - Representing the upper-left portion of a 3x3 2D transform matrix
        // NOTE: Matrix2 cannot represent 2D translation — for that you need Matrix3
        // with homogeneous coordinates (same reason 3D translation needs Matrix4).
        

        // =========================================================
        // Factory functions (construction)
        // =========================================================

        // Returns the identity matrix: ones on the diagonal, zeros elsewhere.
        // Multiplying any matrix or vector by the identity leaves it unchanged.
        // This is the "neutral element" of matrix multiplication (like 1 for numbers).
        inline Matrix2 Identity() {
            return Matrix2(1.0f);
        }

        // Returns a matrix with all components set to zero.
        // Multiplying anything by the zero matrix always produces a zero result.
        inline Matrix2 Zero() {
            return Matrix2(0.0f);
        }

        // Builds a 2D rotation matrix for the given angle (in radians).
        // Rotating a Vector2 by this matrix spins it counter-clockwise around the origin.
        inline Matrix2 Rotation(float radians) {
            float c = std::cos(radians);
            float s = std::sin(radians);
            return Matrix2(
                c, s,  // column 0
                -s, c   // column 1
            );
        }

        // Builds a 2D non-uniform scale matrix.
        // Scales x and y independently — useful for stretching/squashing in 2D.
        inline Matrix2 Scale(float scaleX, float scaleY) {
            return Matrix2(
                scaleX, 0.0f,
                0.0f, scaleY
            );
        }

        // =========================================================
        // Basic operations
        // =========================================================

        // Adds two matrices component-wise
        inline Matrix2 Add(const Matrix2& a, const Matrix2& b) {
            return a + b;
        }

        // Subtracts two matrices component-wise
        inline Matrix2 Subtract(const Matrix2& a, const Matrix2& b) {
            return a - b;
        }

        // Matrix multiplication: combines two transformations into one.
        // Order matters: Multiply(a, b) applies b FIRST, then a.
        // (Because v' = a * b * v means b is applied closest to v first.)
        inline Matrix2 Multiply(const Matrix2& a, const Matrix2& b) {
            return a * b;
        }

        // Scales all components of a matrix by a scalar.
        // Note: this is NOT the same as building a scale transformation matrix.
        inline Matrix2 Scale(const Matrix2& m, float scalar) {
            return m * scalar;
        }

        // Transforms a 2D vector by this matrix.
        // Applies the linear transformation (rotation/scale) encoded in the matrix to v.
        inline Vector2 Transform_vector(const Matrix2& m, const glm::vec2& v) {
            return m * v;
        }

        // =========================================================
        // Matrix properties
        // =========================================================

        // Transposes the matrix: swaps rows and columns.
        // For pure rotation matrices, the transpose equals the inverse (much cheaper).
        // Also used to convert between row-major and column-major layouts.
        inline Matrix2 Transpose(const Matrix2& m) {
            return glm::transpose(m);
        }

        // Returns the determinant: a scalar that encodes how the matrix scales area.
        //   - det > 0: transformation preserves orientation
        //   - det < 0: transformation flips orientation (like a mirror)
        //   - det == 0: matrix is singular (no inverse exists, transformation collapses space)
        inline float Determinant(const Matrix2& m) {
            return glm::determinant(m);
        }

        // Returns the inverse matrix: applying M then Inverse(M) gives the identity.
        // Only valid if Determinant != 0. If it is 0, the matrix is singular and
        // cannot be inverted (the transformation has lost a dimension).
        inline Matrix2 Inverse(const Matrix2& m) {
            return glm::inverse(m);
        }

        // Checks whether the matrix is approximately the identity (within epsilon)
        inline bool Is_identity(const Matrix2& m, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(m[0][0] - 1.0f) < epsilon &&
                std::abs(m[0][1] - 0.0f) < epsilon &&
                std::abs(m[1][0] - 0.0f) < epsilon &&
                std::abs(m[1][1] - 1.0f) < epsilon;
        }


        // Checks whether the matrix is singular (determinant ~0, no inverse exists)
        inline bool Is_singular(const Matrix2& m, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(Determinant(m)) < epsilon;
        }

        // Checks if the matrix is orthogonal: rows and columns are all unit-length
        // and mutually perpendicular. Pure rotation matrices are always orthogonal.
        // For orthogonal matrices: Inverse == Transpose (much cheaper to compute).
        inline bool IsOrthogonal(const Matrix2& m, float epsilon = Constants::EPSILON_SMALL) {
            Matrix2 shouldBeIdentity = m * glm::transpose(m);
            return Is_identity(shouldBeIdentity, epsilon);
        }

        // =========================================================
        // Comparison
        // =========================================================

        // Checks if two matrices are approximately equal component-wise
        inline bool Equals(const Matrix2& a, const Matrix2& b, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(a[0][0] - b[0][0]) < epsilon &&
                std::abs(a[0][1] - b[0][1]) < epsilon &&
                std::abs(a[1][0] - b[1][0]) < epsilon &&
                std::abs(a[1][1] - b[1][1]) < epsilon;
        }

    }
}