#pragma once

#include <Matrix.hpp>
#include <Matrix3.hpp>
#include <Vector.hpp>
#include <Vector3.hpp>
#include <MathConstants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cassert>
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
            return glm::rotate(Matrix4(1.0f), radians, Vec3::UnitX());
        }

        // Builds a rotation matrix around the Y axis (yaw)
        inline Matrix4 RotationY(float radians) {
            return glm::rotate(Matrix4(1.0f), radians, Vec3::UnitY());
        }

        // Builds a rotation matrix around the Z axis (roll)
        inline Matrix4 RotationZ(float radians) {
            return glm::rotate(Matrix4(1.0f), radians, Vec3::UnitZ());
        }

        // Builds a rotation matrix around an arbitrary axis (Rodrigues formula).
        // Reuse this matrix when you need to rotate many vectors by the same
        // axis/angle — much cheaper than calling RotateAroundAxis per vector.
        inline Matrix4 Rotation_axis_angle(const Vector3& axis, float radians) {
            return glm::rotate(Matrix4(1.0f), radians, axis);
        }

        // Builds a rotation matrix from Euler angles (pitch=X, yaw=Y, roll=Z).
        // Applied to a vector, the rotations happen in the order X (pitch) first,
        // then Y (yaw), then Z (roll), all around the FIXED world axes:
        //   R = Rz * Ry * Rx
        // This is the same convention as Quat::From_euler (glm), so both ways of
        // building a rotation from the same angles agree:
        //   Rotation_euler(p, y, r) == Quat::To_matrix4(Quat::From_euler(p, y, r))
        // Different engines use different orders; this is the one the whole Math
        // module (and Transform_Component) uses. With this order the gimbal lock
        // sits at yaw = +-90 degrees.
        inline Matrix4 Rotation_euler(float pitch, float yaw, float roll) {
            Matrix4 rX = RotationX(pitch);
            Matrix4 rY = RotationY(yaw);
            Matrix4 rZ = RotationZ(roll);
            return rZ * rY * rX;
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
        // Orthonormal world space basis of a view: the one the view matrix
        // rotates by, and the one the culling planes are built from.
        struct View_basis {
            Vector3 right;
            Vector3 up;
            Vector3 forward;
        };

        // Builds the basis from a view direction and an approximate up:
        // forward is normalized, right = cross(forward, up) and the up
        // vector is re-orthogonalized from both. Same math as glm::lookAtRH,
        // but it never produces NaN:
        //   - a zero-length forward (eye == center) has no direction, so the
        //     default forward (-Z) is used
        //   - an up that is zero or parallel to forward (looking straight up or
        //     down against world-up) is replaced by an arbitrary perpendicular
        //     of forward (see Vec3::Safe_up)
        inline View_basis Make_view_basis(const Vector3& forward, const Vector3& up)
        {
            View_basis basis;
            basis.forward = Vec3::Normalize(forward);
            if (Vec3::Length_squared(basis.forward) == 0.0f) basis.forward = Vec3::Forward();
            basis.right = Vec3::Normalize(Vec3::Cross(basis.forward, Vec3::Safe_up(basis.forward, up)));
            basis.up = Vec3::Cross(basis.right, basis.forward);
            return basis;
        }

        // View matrix (right-handed, looking down -Z) from an eye position
        // and a basis: transforms world space into camera space.
        inline Matrix4 View_from_basis(const Vector3& eye, const View_basis& basis)
        {
            Matrix4 view(1.0f);
            view[0][0] = basis.right.x;     view[1][0] = basis.right.y;     view[2][0] = basis.right.z;
            view[0][1] = basis.up.x;        view[1][1] = basis.up.y;        view[2][1] = basis.up.z;
            view[0][2] = -basis.forward.x;  view[1][2] = -basis.forward.y;  view[2][2] = -basis.forward.z;
            view[3][0] = -glm::dot(basis.right, eye);
            view[3][1] = -glm::dot(basis.up, eye);
            view[3][2] = glm::dot(basis.forward, eye);
            return view;
        }

        // Builds a View matrix using the "look at" convention.
        // Transforms world-space positions into camera space (eye at origin,
        // looking down -Z). Used to position and orient the camera.
        //   eye    -> where the camera is in world space
        //   center -> the point the camera is looking at
        //   up     -> which direction is "up" for the camera (usually world Y)
        inline Matrix4 Look_at(const Vector3& eye,const Vector3& center,const Vector3& up) 
        {
            return View_from_basis(eye, Make_view_basis(center - eye, up));
        }

        // ---------------------------------------------------------
        // Projection input validation
        // ---------------------------------------------------------
        // A projection built from invalid parameters (aspect 0, fov 0 or PI,
        // near 0, an empty ortho box...) holds inf/NaN, and a single such matrix
        // in a per-frame buffer poisons everything drawn with it. So the
        // projection builders check their input:
        //   - DEBUG builds: an assert fires, to catch the bad caller;
        //   - RELEASE builds: the value is clamped to the nearest valid one (NaN
        //     falls back to a default), so the frame still renders sanely.
        namespace Detail
        {
            // std::clamp that maps NaN to a fallback instead of letting it through.
            inline float Clamp_or(float value, float lo, float hi, float fallback) {
                return std::isnan(value) ? fallback : std::clamp(value, lo, hi);
            }

            // Vertical field of view, valid in (0, PI) radians (tan(fov/2) must stay finite and positive).
            inline float Valid_fov(float fovY) {
                assert(fovY > 0.0f && fovY < Constants::PI && "fovY must be in (0, PI) radians");
                return Clamp_or(fovY, Constants::EPSILON_LARGE, Constants::PI - Constants::EPSILON_LARGE,
                    Constants::FOV_DEFAULT * Constants::DEG_TO_RAD);
            }

            // Width / height, valid when finite and > 0.
            inline float Valid_aspect(float aspect) {
                assert(aspect > 0.0f && std::isfinite(aspect) && "aspect must be finite and > 0");
                return Clamp_or(aspect, Constants::EPSILON_LARGE, 1.0f / Constants::EPSILON_LARGE, 1.0f);
            }

            // Perspective near plane, valid when finite and > 0 (z_ndc is divided by it).
            inline float Valid_near(float nearPlane) {
                assert(nearPlane > 0.0f && std::isfinite(nearPlane) && "near plane must be finite and > 0");
                return Clamp_or(nearPlane, Constants::EPSILON, Constants::FLOAT_MAX, Constants::NEAR_PLANE_DEFAULT);
            }

            // Perspective far plane, valid when finite and beyond the (already valid) near plane.
            inline float Valid_far(float nearPlane, float farPlane) {
                assert(farPlane > nearPlane && std::isfinite(farPlane) && "far plane must be finite and > near plane");
                const float min_far = nearPlane * (1.0f + Constants::EPSILON_SMALL);
                return std::min(farPlane > min_far ? farPlane : min_far, Constants::FLOAT_MAX); // NaN -> min_far
            }

            // Orthographic range [lo, hi]: glm divides by (hi - lo), so the two ends must differ.
            // Only (nearly) equal ends are touched; a flipped range (hi < lo) is legal.
            inline float Keep_apart(float lo, float hi) {
                assert(std::isfinite(lo) && std::isfinite(hi) && lo != hi && "ortho range must be finite and non-empty");
                const float min_gap = std::max(Constants::EPSILON, std::abs(lo) * Constants::EPSILON_SMALL);
                return (std::abs(hi - lo) >= min_gap) ? hi : lo + min_gap;
            }
        }

        // Builds a perspective projection matrix.
        // Simulates how a real camera sees: objects farther away appear smaller.
        // This is the standard projection for 3D games.
        //   fovY       -> vertical field of view in radians (e.g. glm::radians(60.0f)), in (0, PI)
        //   aspect     -> viewport width / viewport height, > 0
        //   nearPlane  -> closest distance the camera can see, > 0 (avoid ~0, causes precision issues)
        //   farPlane   -> farthest distance the camera can see, > nearPlane
        // Invalid values assert in debug builds and are clamped in release builds.
        inline Matrix4 Perspective(float fovY, float aspect,
            float nearPlane, float farPlane) {
            nearPlane = Detail::Valid_near(nearPlane);
            return glm::perspective(Detail::Valid_fov(fovY), Detail::Valid_aspect(aspect),
                nearPlane, Detail::Valid_far(nearPlane, farPlane));
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
        // gains exactly nothing. The requirement is enforced by the
        // Renderer, not assumed: Vulkan_Device only selects GPUs that offer
        // D32_SFLOAT or D32_SFLOAT_S8_UINT as a depth attachment
        // (Device_Support::depth_format, Vulkan_Device::Get_depth_format).
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
        //   fovY      -> vertical field of view in radians, in (0, PI)
        //   aspect    -> viewport width / viewport height, > 0
        //   nearPlane -> closest distance the camera can see (must be > 0)
        // Invalid values assert in debug builds and are clamped in release
        // builds (aspect 0 or near 0 would otherwise put inf/NaN in the matrix).
        //
        // Right-handed, camera looking down -Z, [0,1] depth: the same
        // conventions glm::perspective follows under GLM_FORCE_DEPTH_ZERO_TO_ONE.
        inline Matrix4 Perspective_reverse_z_infinite(float fovY, float aspect,
            float nearPlane) {
            fovY = Detail::Valid_fov(fovY);
            aspect = Detail::Valid_aspect(aspect);
            nearPlane = Detail::Valid_near(nearPlane);

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
        // Each pair (left/right, bottom/top, near/far) must differ, or the matrix
        // would divide by zero: equal values assert in debug builds and are pushed
        // apart by a minimal gap in release builds. A flipped pair (right < left)
        // is legal and mirrors the projection.
        inline Matrix4 Orthographic(float left, float right,
            float bottom, float top,
            float nearPlane, float farPlane) {
            right = Detail::Keep_apart(left, right);
            top = Detail::Keep_apart(bottom, top);
            farPlane = Detail::Keep_apart(nearPlane, farPlane);
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

        // Returns the normal matrix: inverse-transpose of the upper-left 3x3 of m.
        // CRITICAL for correct lighting with non-uniform scale: normals cannot
        // be transformed by the same matrix as positions — they need this one.
        // Assumes an affine matrix (TRS), where translation does not affect
        // normals. Delegates to Mat3::Normal_matrix, which stays finite when an
        // axis is scaled to 0 (see there) and only needs the 3x3, so it is also
        // cheaper than inverting the full 4x4 (this runs once per draw).
        inline Matrix3 Normal_matrix(const Matrix4& m) {
            return Mat3::Normal_matrix(Matrix3(m));
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

        // Checks if the matrix is singular (no inverse exists).
        // The determinant alone cannot decide this: it grows and shrinks with the
        // SIZE of the matrix (Scale_uniform(0.1) has det 1e-3 and is perfectly
        // invertible), so an absolute threshold on it flags valid small matrices.
        // Instead each column is normalized to unit length first; the determinant
        // of that matrix is the Hadamard ratio, in [0, 1]: 1 when the columns are
        // mutually perpendicular, 0 when they are linearly dependent, whatever
        // their size. "epsilon" is a threshold on that ratio. A zero column, or a
        // matrix with NaN/inf components, counts as singular.
        //
        // An affine matrix (last row 0,0,0,1, i.e. any TRS) is singular exactly
        // when its upper 3x3 is, so only that part is tested: the translation
        // column would otherwise inflate the column lengths and make a far-away
        // object look singular.
        inline bool Is_singular(const Matrix4& m, float epsilon = Constants::EPSILON_SMALL) {
            const bool affine = m[0][3] == 0.0f && m[1][3] == 0.0f && m[2][3] == 0.0f && m[3][3] == 1.0f;
            if (affine) return Mat3::Is_singular(Matrix3(m), epsilon);

            const float len0 = glm::length(m[0]);
            const float len1 = glm::length(m[1]);
            const float len2 = glm::length(m[2]);
            const float len3 = glm::length(m[3]);
            if (!(len0 > 0.0f) || !(len1 > 0.0f) || !(len2 > 0.0f) || !(len3 > 0.0f)) return true; // also catches NaN

            const float ratio = std::abs(Determinant(Matrix4(m[0] / len0, m[1] / len1, m[2] / len2, m[3] / len3)));
            return !(ratio >= epsilon); // written this way so NaN counts as singular
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
        // If the matrix MIRRORS (determinant of its 3x3 is negative) the X scale
        // is returned NEGATIVE: a rotation cannot represent a mirror, so the usual
        // convention for decomposing a TRS matrix is to carry it in the sign of
        // one scale axis. This makes the round trip work:
        //   TRS(Get_translation(m), quat_from(Get_Rotation(m)), Get_scale(m)) == m
        inline Vector3 Get_scale(const Matrix4& m) {
            Vector3 scale(
                glm::length(glm::vec3(m[0])),
                glm::length(glm::vec3(m[1])),
                glm::length(glm::vec3(m[2]))
            );
            if (glm::determinant(Matrix3(m)) < 0.0f) scale.x = -scale.x;
            return scale;
        }

        // Extracts the rotation component as a 3x3 matrix from a TRS matrix,
        // removing the scale by normalizing the column vectors.
        // Always returns a proper rotation (determinant +1): a mirror in m is
        // carried by the sign of Get_scale().x, not by the rotation.
        // An axis scaled to 0 has lost its direction, so it is rebuilt as the
        // cross product of the other two. If two or more axes collapsed the
        // rotation cannot be recovered and the identity is returned.
        inline Matrix3 Get_Rotation(const Matrix4& m) {
            Vector3 axis[3] = { Vector3(m[0]), Vector3(m[1]), Vector3(m[2]) };
            bool valid[3];
            int valid_count = 0;

            for (int i = 0; i < 3; ++i)
            {
                const float len_sq = glm::dot(axis[i], axis[i]);
                valid[i] = len_sq > Constants::FLOAT_MIN;
                if (valid[i])
                {
                    axis[i] *= 1.0f / std::sqrt(len_sq);
                    ++valid_count;
                }
            }

            if (valid_count < 2) return Matrix3(1.0f);

            if (valid_count == 3)
            {
                // A mirror shows up as a left-handed basis: flip one axis to get a rotation.
                if (glm::dot(axis[0], glm::cross(axis[1], axis[2])) < 0.0f) axis[0] = -axis[0];
            }
            else if (!valid[0]) axis[0] = glm::cross(axis[1], axis[2]);
            else if (!valid[1]) axis[1] = glm::cross(axis[2], axis[0]);
            else                axis[2] = glm::cross(axis[0], axis[1]);

            return Matrix3(axis[0], axis[1], axis[2]);
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