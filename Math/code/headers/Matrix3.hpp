#pragma once
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <Matrix.hpp>
#include <Vector.hpp>
#include <MathConstants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <cmath>

namespace MathLib 
{
    
    namespace Mat3 
    {
        // 3x3 matrix, primarily used for:
        //   - 2D transformations WITH translation (homogeneous 2D coordinates)
        //   - 3D rotations and scales WITHOUT translation
        //   - Normal matrix: transforms surface normals correctly when the model
        //     has non-uniform scale (it's the inverse-transpose of the upper-left
        //     3x3 of the ModelView matrix)
        

        // =========================================================
        // Factory functions (construction)
        // =========================================================

        // Identity matrix: the neutral element of matrix multiplication
        inline Matrix3 Identity() {
            return Matrix3(1.0f);
        }

        // Zero matrix: all components set to zero
        inline Matrix3 Zero() {
            return Matrix3(0.0f);
        }

        // Builds a 3D rotation matrix around the X axis (pitch).
        // Rotates the Y axis toward Z.
        inline Matrix3 RotationX(float radians) {
            float c = std::cos(radians);
            float s = std::sin(radians);
            return Matrix3(
                1.0f, 0.0f, 0.0f,
                0.0f, c, s,
                0.0f, -s, c
            );
        }

        // Builds a 3D rotation matrix around the Y axis (yaw).
        // Rotates the Z axis toward X.
        inline Matrix3 RotationY(float radians) {
            float c = std::cos(radians);
            float s = std::sin(radians);
            return Matrix3(
                c, 0.0f, -s,
                0.0f, 1.0f, 0.0f,
                s, 0.0f, c
            );
        }

        // Builds a 3D rotation matrix around the Z axis (roll).
        // Rotates the X axis toward Y.
        inline Matrix3 RotationZ(float radians) {
            float c = std::cos(radians);
            float s = std::sin(radians);
            return Matrix3(
                c, s, 0.0f,
                -s, c, 0.0f,
                0.0f, 0.0f, 1.0f
            );
        }

        // Builds a 3D rotation matrix around an arbitrary axis (Rodrigues formula).
        // Same concept as RotateAroundAxis in Vector3, but stored as a matrix
        // so it can be reused efficiently for many vectors without recomputing.
        inline Matrix3 Rotation_axis_angle(const Vector3& axis, float radians) {
            return Matrix3(glm::rotate(glm::mat4(1.0f), radians, axis));
        }

        // Builds a 3D non-uniform scale matrix
        inline Matrix3 Scale(float scaleX, float scaleY, float scaleZ) {
            return Matrix3(
                scaleX, 0.0f, 0.0f,
                0.0f, scaleY, 0.0f,
                0.0f, 0.0f, scaleZ
            );
        }

        // Builds a uniform scale matrix (same scale on all axes)
        inline Matrix3 Scale_uniform(float scale) {
            return Scale(scale, scale, scale);
        }

        // =========================================================
        // Basic operations
        // =========================================================

        // Adds two matrices component-wise
        inline Matrix3 Add(const Matrix3& a, const Matrix3& b) {
            return a + b;
        }

        // Subtracts two matrices component-wise
        inline Matrix3 Subtract(const Matrix3& a, const Matrix3& b) {
            return a - b;
        }

        // Matrix multiplication: combines two transformations.
        // Order matters: Multiply(a, b) applies b FIRST, then a.
        inline Matrix3 Multiply(const Matrix3& a, const Matrix3& b) {
            return a * b;
        }

        // Scales all components by a scalar (NOT a scale transformation)
        inline Matrix3 Scale(const Matrix3& m, float scalar) {
            return m * scalar;
        }

        // Transforms a 3D vector by this matrix (applies rotation + scale)
        inline glm::vec3 Transform_vector(const Matrix3& m, const Vector3& v) {
            return m * v;
        }

        // =========================================================
        // Matrix properties
        // =========================================================

        // Transposes the matrix: swaps rows and columns.
        // For orthogonal matrices (pure rotations), transpose == inverse.
        // For normal matrices in lighting: the normal matrix IS the
        // inverse-transpose of the model matrix's upper 3x3.
        inline Matrix3 Transpose(const Matrix3& m) {
            return glm::transpose(m);
        }

        // Returns the determinant. Encodes how the matrix scales volume:
        //   - det > 0: preserves orientation
        //   - det < 0: flips orientation
        //   - det == 0: singular, no inverse
        inline float Determinant(const Matrix3& m) {
            return glm::determinant(m);
        }

        // Returns the inverse matrix. Applying M then Inverse(M) gives identity.
        // Only valid when Determinant != 0.
        inline Matrix3 Inverse(const Matrix3& m) {
            return glm::inverse(m);
        }

        // Returns the normal matrix: inverse-transpose of m.
        // CRITICAL for correct lighting: when a mesh has non-uniform scale,
        // normals cannot be transformed by the same matrix as positions —
        // they must use this matrix instead, otherwise lighting breaks visually.
        inline Matrix3 Normal_matrix(const Matrix3& m) {
            return glm::transpose(glm::inverse(m));
        }

        // Checks if the matrix is approximately equal to identity
        inline bool Is_identity(const Matrix3& m, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(m[0][0] - 1.0f) < epsilon &&
                std::abs(m[0][1] - 0.0f) < epsilon &&
                std::abs(m[0][2] - 0.0f) < epsilon &&
                std::abs(m[1][0] - 0.0f) < epsilon &&
                std::abs(m[1][1] - 1.0f) < epsilon &&
                std::abs(m[1][2] - 0.0f) < epsilon &&
                std::abs(m[2][0] - 0.0f) < epsilon &&
                std::abs(m[2][1] - 0.0f) < epsilon &&
                std::abs(m[2][2] - 1.0f) < epsilon;
        }


        // Checks if the matrix is singular (no inverse exists)
        inline bool Is_singular(const Matrix3& m, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(Determinant(m)) < epsilon;
        }

        // Checks if the matrix is orthogonal (pure rotation, no scale or shear).
        // For orthogonal matrices: Inverse == Transpose (much cheaper).
        inline bool IsOrthogonal(const Matrix3& m, float epsilon = Constants::EPSILON_SMALL) {
            Matrix3 shouldBeIdentity = m * glm::transpose(m);
            return Is_identity(shouldBeIdentity, epsilon);
        }

        // =========================================================
        // Comparison
        // =========================================================

        // Checks if two matrices are approximately equal component-wise
        inline bool Equals(const Matrix3& a, const Matrix3& b, float epsilon = Constants::EPSILON_SMALL) {
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 3; ++row)
                    if (std::abs(a[col][row] - b[col][row]) >= epsilon) return false;
            return true;
        }
    }
}