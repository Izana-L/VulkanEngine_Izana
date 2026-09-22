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
    
    namespace Mat4
    {
        // 4x4 matrix: the workhorse of 3D graphics.
        // Represents ANY combination of translation, rotation, scale, and projection
        // using homogeneous coordinates (w component).
        // Every object in a 3D scene has a 4x4 Model matrix.
        // The camera has a 4x4 View matrix.
        // The projection (perspective/ortho) is a 4x4 Projection matrix.
        // The GPU multiplies these together (MVP) to transform every vertex.

        using Matrix3 = Matrix< 3, 3, float >;
        // =========================================================
        // Factory functions (construction)
        // =========================================================

        // Identity matrix: the neutral element of matrix multiplication.
        // Applying this to a vertex leaves it completely unchanged.
        inline Matrix4 Identity() {
            return Matrix4(1.0f);
        }

        // Zero matrix: all components set to zero
        inline Matrix4 Zero() {
            return Matrix4(0.0f);
        }

        // Builds a translation matrix.
        // Applying this to a point moves it by (tx, ty, tz) in world space.
        // Translation only affects Vector4 with w=1 (points), not w=0 (directions).
        inline Matrix4 Translation(float tx, float ty, float tz) {
            return glm::translate(Matrix4(1.0f), glm::vec3(tx, ty, tz));
        }

        inline Matrix4 Translation(const glm::vec3& t) {
            return glm::translate(Matrix4(1.0f), t);
        }

        // Builds a rotation matrix around the X axis (pitch)
        inline Matrix4 RotationX(float radians) {
            return glm::rotate(Matrix4(1.0f), radians, glm::vec3(1.0f, 0.0f, 0.0f));
        }

        // Builds a rotation matrix around the Y axis (yaw)
        inline Matrix4 RotationY(float radians) {
            return glm::rotate(Matrix4(1.0f), radians, glm::vec3(0.0f, 1.0f, 0.0f));
        }

        // Builds a rotation matrix around the Z axis (roll)
        inline Matrix4 RotationZ(float radians) {
            return glm::rotate(Matrix4(1.0f), radians, glm::vec3(0.0f, 0.0f, 1.0f));
        }

        // Builds a rotation matrix around an arbitrary axis (Rodrigues formula).
        // Reuse this matrix when you need to rotate many vectors by the same
        // axis/angle — much cheaper than calling RotateAroundAxis per vector.
        inline Matrix4 Rotation_axis_angle(const Vector3& axis, float radians) {
            return glm::rotate(Matrix4(1.0f), radians, axis);
        }

        // Builds a rotation matrix from Euler angles (pitch=X, yaw=Y, roll=Z).
        // Applied in ZYX order (roll first, then yaw, then pitch) — be aware that
        // different engines use different orders; always document which order you use.
        inline Matrix4 Rotation_euler(float pitch, float yaw, float roll) {
            Matrix4 rX = RotationX(pitch);
            Matrix4 rY = RotationY(yaw);
            Matrix4 rZ = RotationZ(roll);
            return rX * rY * rZ;
        }

        // Builds a non-uniform scale matrix
        inline Matrix4 Scale(float scaleX, float scaleY, float scaleZ) {
            return glm::scale(Matrix4(1.0f), glm::vec3(scaleX, scaleY, scaleZ));
        }

        inline Matrix4 Scale(const Vector3& scale) {
            return glm::scale(Matrix4(1.0f), scale);
        }

        // Builds a uniform scale matrix (same scale on all axes)
        inline Matrix4 Scale_uniform(float scale) {
            return glm::scale(Matrix4(1.0f), glm::vec3(scale, scale, scale));
        }

        // Builds a complete TRS (Translation * Rotation * Scale) model matrix.
        // This is how every 3D object's transform is typically stored and applied.
        // Order is critical: scale first, then rotate, then translate —
        // doing it in any other order produces unexpected results.
        inline Matrix4 TRS(const Vector3& translation,
            const glm::quat& rotation,
            const Vector3& scale) {
            Matrix4 t = Translation(translation);
            Matrix4 r = glm::mat4_cast(rotation);
            Matrix4 s = Scale(scale);
            return t * r * s;
        }

        // =========================================================
        // View and projection matrices
        // =========================================================

        // Builds a View matrix using the "look at" convention.
        // Transforms world-space positions into camera space (eye at origin,
        // looking down -Z). Used to position and orient the camera.
        //   eye    -> where the camera is in world space
        //   center -> the point the camera is looking at
        //   up     -> which direction is "up" for the camera (usually world Y)
        inline Matrix4 Look_at(const Vector3& eye,
            const Vector3& center,
            const Vector3& up) {
            return glm::lookAt(eye, center, up);
        }

        // Builds a perspective projection matrix.
        // Simulates how a real camera sees: objects farther away appear smaller.
        // This is the standard projection for 3D games.
        //   fovY       -> vertical field of view in radians (e.g. glm::radians(60.0f))
        //   aspect     -> viewport width / viewport height
        //   nearPlane  -> closest distance the camera can see (avoid 0, causes precision issues)
        //   farPlane   -> farthest distance the camera can see
        inline Matrix4 Perspective(float fovY, float aspect,
            float nearPlane, float farPlane) {
            return glm::perspective(fovY, aspect, nearPlane, farPlane);
        }
        // =========================================================
        // Reverse-Z
        // =========================================================

        // Reverse-Z correction: maps clip-space z to (w - z), which after the
        // perspective divide turns a [0,1] depth range into [1,0]. The near
        // plane lands on 1.0 and the far plane on 0.0.
        //
        // Why bother: float32 packs most of its representable values near
        // 0.0, and a perspective projection packs most of ITS resolution near
        // the camera. In the standard mapping both pile up in the same place,
        // so the far half of the frustum is starved and z-fights. Reversing
        // the range makes the two distributions cancel, giving nearly uniform
        // relative precision across the whole frustum.
        //
        // REQUIRES a floating-point depth buffer (VK_FORMAT_D32_SFLOAT).
        // On a UNORM depth buffer the spacing is already uniform and this
        // gains exactly nothing.
        //
        // Multiply on the LEFT of a standard [0,1] projection:
        //   reversed = Reverse_z_correction() * standard;
        //
        // Callers must also clear depth to 0.0 and use a GREATER compare op.
        // All three go together or the scene vanishes.
        inline Matrix4 Reverse_z_correction() {
            Matrix4 correction(1.0f);
            correction[2][2] = -1.0f;   // z' = -z ...
            correction[3][2] = 1.0f;   // ... + w
            return correction;
        }

        // Reverse-Z perspective projection with an INFINITE far plane, built
        // directly in its final form (no correction matrix needed).
        //
        // The depth mapping collapses to:
        //     z_ndc = nearPlane / distance_from_camera
        // so the near plane is 1.0 and infinity approaches 0.0, never
        // reaching it. Nothing is ever clipped for being too far away.
        //
        // With the far plane gone there is no far/near ratio left to burn
        // precision on: nearPlane is the ONLY knob that affects depth
        // resolution. Raise it as high as the game tolerates.
        //
        // Same requirements as Reverse_z_correction(): float depth buffer,
        // depth cleared to 0.0, compare op GREATER.
        //
        //   fovY      -> vertical field of view in radians
        //   aspect    -> viewport width / viewport height
        //   nearPlane -> closest distance the camera can see (must be > 0)
        //
        // Right-handed, camera looking down -Z, [0,1] depth: the same
        // conventions glm::perspective follows under GLM_FORCE_DEPTH_ZERO_TO_ONE.
        inline Matrix4 Perspective_reverse_z_infinite(float fovY, float aspect,
            float nearPlane) {
            const float focal = 1.0f / std::tan(fovY * 0.5f);

            Matrix4 projection(0.0f);
            projection[0][0] = focal / aspect;
            projection[1][1] = focal;
            projection[2][2] = 0.0f;        // far plane at infinity -> 0.0
            projection[2][3] = -1.0f;       // w_clip = -z_view
            projection[3][2] = nearPlane;   // near plane -> 1.0
            return projection;
        }

        // Builds an orthographic projection matrix.
        // No perspective foreshortening: objects stay the same size regardless of depth.
        // Used for 2D games, UI rendering, shadow maps, and technical/CAD views.
        inline Matrix4 Orthographic(float left, float right,
            float bottom, float top,
            float nearPlane, float farPlane) {
            return glm::ortho(left, right, bottom, top, nearPlane, farPlane);
        }

        // =========================================================
        // Basic operations
        // =========================================================

        // Adds two matrices component-wise
        inline Matrix4 Add(const Matrix4& a, const Matrix4& b) {
            return a + b;
        }

        // Subtracts two matrices component-wise
        inline Matrix4 Subtract(const Matrix4& a, const Matrix4& b) {
            return a - b;
        }

        // Matrix multiplication: combines two transformations into one matrix.
        // Order matters: Multiply(a, b) applies b FIRST, then a.
        // Common usage: MVP = Projection * View * Model
        // (Model applied first to vertex, then View, then Projection)
        inline Matrix4 Multiply(const Matrix4& a, const Matrix4& b) {
            return a * b;
        }

        // Scales all components by a scalar (NOT a scale transformation matrix)
        inline Matrix4 Scale(const Matrix4& m, float scalar) {
            return m * scalar;
        }

        // Transforms a 4D vector by this matrix.
        // Use FromPoint(v3) for points (w=1, translation applies)
        // Use FromDirection(v3) for directions (w=0, translation ignored)
        inline Vector4 Transform_vector(const Matrix4& m, const Vector4& v) {
            return m * v;
        }

        // Transforms a POINT (w = 1): rotation, scale and translation apply.
        inline Vector3 Transform_point(const Matrix4& m, const Vector3& p) {
            return Vector3(m * Vector4(p, 1.0f));
        }

        // Transforms a DIRECTION (w = 0): translation is ignored and the
        // result is re-normalized, so a scaled matrix still yields a unit
        // vector. A degenerate matrix (zero scale on that axis) returns the
        // input unchanged rather than NaN.
        inline Vector3 Transform_direction(const Matrix4& m, const Vector3& d) {
            const Vector3 transformed = Vector3(m * Vector4(d, 0.0f));
            const float   length = glm::length(transformed);
            return (length > Constants::EPSILON) ? transformed / length : d;
        }

        // =========================================================
        // Matrix properties
        // =========================================================

        // Transposes the matrix: swaps rows and columns.
        // For orthogonal matrices (pure rotations), transpose == inverse (much cheaper).
        inline Matrix4 Transpose(const Matrix4& m) {
            return glm::transpose(m);
        }

        // Returns the determinant. Encodes how the matrix scales volume:
        //   - det > 0: preserves orientation
        //   - det < 0: flips orientation (mirrored)
        //   - det == 0: singular, no inverse
        inline float Determinant(const Matrix4& m) {
            return glm::determinant(m);
        }

        // Returns the inverse matrix: applying M then Inverse(M) gives identity.
        // Matrix inversion is expensive — cache the result if you need it multiple times.
        // For pure rotation matrices use Transpose instead (much cheaper, same result).
        inline Matrix4 Inverse(const Matrix4& m) {
            return glm::inverse(m);
        }

        // Returns the normal matrix: inverse-transpose of m as a Matrix3.
        // CRITICAL for correct lighting with non-uniform scale: normals cannot
        // be transformed by the same matrix as positions — they need this one.
        inline Matrix3 Normal_matrix(const Matrix4& m) {
            return Matrix3(glm::transpose(glm::inverse(m)));
        }

        // Extracts only the upper-left 3x3 submatrix (rotation + scale, no translation).
        // Useful when you need to transform directions without applying translation.
        inline Matrix3 To_matrix3(const Matrix4& m) {
            return Matrix3(m);
        }

        // Checks if the matrix is approximately equal to identity
        inline bool Is_identity(const Matrix4& m, float epsilon = Constants::EPSILON_SMALL) {
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row) {
                    float expected = (col == row) ? 1.0f : 0.0f;
                    if (std::abs(m[col][row] - expected) >= epsilon) return false;
                }
            return true;
        }

        // Checks if the matrix is singular (no inverse exists)
        inline bool Is_singular(const Matrix4& m, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(Determinant(m)) < epsilon;
        }

        // Checks if the matrix is orthogonal (pure rotation, no scale or shear).
        // For orthogonal matrices: Inverse == Transpose.
        inline bool IsOrthogonal(const Matrix4& m, float epsilon = Constants::EPSILON_SMALL) {
            Matrix4 shouldBeIdentity = m * glm::transpose(m);
            return Is_identity(shouldBeIdentity, epsilon);
        }

        // =========================================================
        // Decomposition
        // =========================================================

        // Extracts the translation component from a TRS matrix.
        // The translation is stored in the last column of the matrix.
        inline Vector3 Get_translation(const Matrix4& m) {
            return glm::vec3(m[3]);
        }

        // Extracts the scale component from a TRS matrix.
        // Scale is the length of each of the first three column vectors.
        inline Vector3 Get_scale(const Matrix4& m) {
            return glm::vec3(
                glm::length(glm::vec3(m[0])),
                glm::length(glm::vec3(m[1])),
                glm::length(glm::vec3(m[2]))
            );
        }

        // Extracts the rotation component as a 3x3 matrix from a TRS matrix,
        // removing the scale by normalizing the column vectors.
        inline Matrix3 Get_Rotation(const Matrix4& m) {
            glm::vec3 scale = Get_scale(m);
            return Matrix3(
                glm::vec3(m[0]) / scale.x,
                glm::vec3(m[1]) / scale.y,
                glm::vec3(m[2]) / scale.z
            );
        }

        // =========================================================
        // Interpolation
        // =========================================================

        // Linearly interpolates between two matrices component-wise.
        // Useful for simple blending, but does NOT produce correct results
        // for rotation matrices (use quaternion slerp for rotations instead).
        inline Matrix4 Lerp(const Matrix4& a, const Matrix4& b, float t) {
            return Matrix4(
                glm::mix(a[0], b[0], t),
                glm::mix(a[1], b[1], t),
                glm::mix(a[2], b[2], t),
                glm::mix(a[3], b[3], t)
            );
        }

        // =========================================================
        // Comparison
        // =========================================================

        // Checks if two matrices are approximately equal component-wise
        inline bool Equals(const Matrix4& a, const Matrix4& b, float epsilon = Constants::EPSILON_SMALL) {
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row)
                    if (std::abs(a[col][row] - b[col][row]) >= epsilon) return false;
            return true;
        }
    }
}