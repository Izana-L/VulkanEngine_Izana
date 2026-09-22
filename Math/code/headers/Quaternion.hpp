#pragma once
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <MathConstants.hpp>
#include <cmath>
#include <Vector.hpp>
#include <Matrix.hpp>

namespace MathLib {


    namespace Quat
    {
        using Quaternion = glm::quat;
        
        // Quaternion: represents a 3D rotation using 4 components (x, y, z, w).
        // Why quaternions instead of Euler angles or matrices?
        //   - No gimbal lock (Euler angles can lose a degree of freedom)
        //   - Cheaper to interpolate smoothly (Slerp) than matrices or Euler angles
        //   - Cheaper to store/compose than a full rotation matrix (4 floats vs 9)
        //   - Numerically stable for repeated rotations (re-normalizing is cheap and safe)
        // Most game engines store object rotations as quaternions internally,
        // even if they expose Euler angles in the editor UI for human readability.


        // =========================================================
        // Factory functions (construction)
        // =========================================================
        // Identity quaternion: represents "no rotation".
        // Applying this to a vector leaves it unchanged.
        inline Quaternion Identity() {
            return Quaternion(1.0f, 0.0f, 0.0f, 0.0f); // w, x, y, z
        }

        // Builds a quaternion representing a rotation of "radians" around "axis".
        // The axis does NOT need to be normalized beforehand; glm handles that.
        // This is the most common way to build a rotation procedurally
        // (e.g. "rotate 90 degrees around the Y axis").
        inline Quaternion From_axis_angle(const Vector3& axis, float radians) {
            return glm::angleAxis(radians, glm::normalize(axis));
        }

        // Builds a quaternion from Euler angles (pitch, yaw, roll), in radians.
        // Useful for exposing rotation in an editor UI, since Euler angles are
        // more intuitive for humans than raw quaternion components.
        // NOTE: convert back and forth carries the usual Euler angle quirks
        // (gimbal lock when reading back, ambiguity near +-90 degrees pitch).
        inline Quaternion From_euler(float pitch, float yaw, float roll) {
            return glm::quat(Vector3(pitch, yaw, roll));
        }

        inline Quaternion From_euler(const Vector3& eulerRadians) {
            return glm::quat(eulerRadians);
        }

        // Builds a quaternion from a 3x3 or 4x4 rotation matrix.
        // Useful when you've computed a rotation as a matrix (e.g. from LookAt)
        // and need to store/interpolate it as a quaternion instead.
        inline Quaternion From_matrix(const Matrix3& m) {
            return glm::quat_cast(m);
        }

        inline Quaternion From_matrix(const Matrix4& m) {
            return glm::quat_cast(m);
        }

        // Builds a quaternion that rotates "from" direction to "to" direction.
        // Useful for orienting an object to face a target, or aligning a vector
        // with a surface normal.
        inline Quaternion From_to_rotation(const Vector3& from, const Vector3& to) {
            Vector3 f = glm::normalize(from);
            Vector3 t = glm::normalize(to);
            float dot = glm::dot(f, t);

            // vectors point in the same direction, no rotation needed
            if (dot >= 1.0f - Constants::EPSILON_SMALL) {
                return Identity();
            }

            // vectors point in opposite directions: pick an arbitrary perpendicular axis
            if (dot <= -1.0f + Constants::EPSILON_SMALL) {
                Vector3 axis = glm::cross(Vector3(1.0f, 0.0f, 0.0f), f);
                if (glm::length2(axis) < Constants::EPSILON_SMALL) {
                    axis = glm::cross(Vector3(0.0f, 1.0f, 0.0f), f);
                }
                return glm::angleAxis(Constants::PI, glm::normalize(axis));
            }

            Vector3 axis = glm::normalize(glm::cross(f, t));
            float angle = std::acos(dot);
            return glm::angleAxis(angle, axis);
        }

        // Builds a quaternion that orients an object to look toward "forward",
        // with "up" defining the roll. Equivalent to a rotation-only LookAt.
        // Useful for making an object face a target (e.g. a turret aiming at a player).
        inline Quaternion Look_rotation(const Vector3& forward, const Vector3& up = Vector3(0.0f, 1.0f, 0.0f)) {
            return glm::quatLookAt(glm::normalize(forward), up);
        }

        // =========================================================
        // Basic operations
        // =========================================================

        // Combines two rotations: applying Multiply(a, b) to a vector applies
        // rotation b FIRST, then rotation a (same order convention as matrices).
        inline Quaternion Multiply(const Quaternion& a, const Quaternion& b) {
            return a * b;
        }

        // Rotates a 3D vector by this quaternion.
        inline Vector3 Rotate_vector(const Quaternion& q, const  Vector3& v) {
            return q * v;
        }

        // Returns the conjugate: same rotation axis, but flips the sign of x,y,z.
        // For a UNIT quaternion, the conjugate equals the inverse (cheaper to compute).
        inline Quaternion Conjugate(const Quaternion& q) {
            return glm::conjugate(q);
        }

        // Returns the inverse rotation: applying Q then Inverse(Q) undoes the rotation.
        // For non-unit quaternions this differs from the conjugate; for unit
        // quaternions (the common case) Inverse and Conjugate give the same result.
        inline Quaternion Inverse(const Quaternion& q) {
            return glm::inverse(q);
        }

        // =========================================================
        // Normalization
        // =========================================================

        // Returns a unit-length quaternion (length == 1).
        // Quaternions MUST stay normalized to represent valid rotations -
        // floating point error accumulates over many operations (e.g. repeated
        // multiplications each frame), so re-normalize periodically to avoid drift.
        inline Quaternion Normalize(const Quaternion& q) {
            return glm::normalize(q);
        }

        // Checks if the quaternion is already unit-length (within tolerance)
        inline bool Is_normalized(const Quaternion& q, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(glm::length(q) - 1.0f) < epsilon;
        }

        // Returns the length (magnitude) of the quaternion.
        // For a valid rotation this should always be ~1; anything else
        // usually indicates accumulated floating point drift.
        inline float Length(const Quaternion& q) {
            return glm::length(q);
        }

        // =========================================================
        // Conversion
        // =========================================================

        // Converts the quaternion to a 3x3 rotation matrix.
        // Use this when you need to combine the rotation with a Matrix4 TRS,
        // or pass it to systems that expect matrices instead of quaternions.
        inline Matrix3 To_matrix3(const Quaternion& q) {
            return glm::mat3_cast(q);
        }

        // Converts the quaternion to a 4x4 rotation matrix (no translation/scale)
        inline Matrix4 To_matrix4(const Quaternion& q) {
            return glm::mat4_cast(q);
        }

        // Converts the quaternion to Euler angles (pitch, yaw, roll), in radians.
        // Useful for displaying rotation in an editor UI.
        // NOTE: this conversion is not perfectly stable - the same rotation can
        // map to different Euler angle results depending on the order/convention,
        // and gimbal lock can occur near +-90 degree pitch values.
        inline Vector3 ToEuler(const Quaternion& q) {
            return glm::eulerAngles(q);
        }

        // Extracts the rotation axis and angle (in radians) from the quaternion.
        // Useful for debugging or for systems that work with axis-angle directly.
        inline void To_axis_angle(const Quaternion& q, Vector3& outAxis, float& outAngle) {
            outAngle = glm::angle(q);
            outAxis = glm::axis(q);
        }

        // =========================================================
        // Interpolation
        // =========================================================

        // Linear interpolation between two quaternions, then re-normalizes.
        // Cheaper than Slerp but does NOT produce constant angular speed -
        // the rotation speeds up/slows down non-uniformly across t.
        // Good enough for small angle differences or non-critical blending,
        // and significantly cheaper to compute than Slerp.
        inline Quaternion Lerp(const Quaternion& a, const Quaternion& b, float t) {
            return glm::normalize(glm::mix(a, b, t));
        }

        // Spherical linear interpolation: the standard way to interpolate
        // between two rotations. Produces constant angular speed and always
        // takes the shortest path around the rotation sphere.
        // This is what you should use for animation blending, camera rotation,
        // and any gameplay code that smoothly rotates an object over time.
        inline Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t) {
            return glm::slerp(a, b, t);
        }

        // Normalized linear interpolation: same as Lerp but the name makes the
        // re-normalization step explicit. Functionally identical to Lerp above;
        // provided because "Nlerp" is the common term used in game engine code.
        inline Quaternion Nlerp(const Quaternion& a, const Quaternion& b, float t) {
            return Lerp(a, b, t);
        }

        // =========================================================
        // Angles between rotations
        // =========================================================

        // Returns the angle (in radians) needed to rotate from quaternion a to b.
        // Useful for checking how "different" two rotations are
        // (e.g. snapping logic, rotation speed limiting).
        inline float Angle_between(const Quaternion& a, const Quaternion& b) {
            float dot = glm::clamp(std::abs(glm::dot(a, b)), -1.0f, 1.0f);
            return 2.0f * std::acos(dot);
        }

        // =========================================================
        // Direction vectors derived from a rotation
        // =========================================================

        // Returns the local "forward" direction after applying this rotation.
        // Assumes -Z is forward in local space (common convention; verify against
        // your engine's coordinate system, some use +Z as forward instead).
        inline Vector3 GetForward(const Quaternion& q) {
            return Rotate_vector(q, Vector3(0.0f, 0.0f, -1.0f));
        }

        // Returns the local "right" direction after applying this rotation
        inline Vector3 GetRight(const Quaternion& q) {
            return Rotate_vector(q, Vector3(1.0f, 0.0f, 0.0f));
        }

        // Returns the local "up" direction after applying this rotation
        inline Vector3 GetUp(const Quaternion& q) {
            return Rotate_vector(q, Vector3(0.0f, 1.0f, 0.0f));
        }

        // =========================================================
        // Comparison and utilities
        // =========================================================

        // Checks if two quaternions are approximately equal.
        // NOTE: a quaternion Q and its negation -Q represent the SAME rotation,
        // so this also checks the negated case to avoid false negatives.
        inline bool Equals(const Quaternion& a, const Quaternion& b, float epsilon = Constants::EPSILON_SMALL) {
            float dot = glm::dot(a, b);
            return std::abs(std::abs(dot) - 1.0f) < epsilon;
        }

        // Checks if the quaternion is approximately the identity (no rotation)
        inline bool Is_identity(const Quaternion& q, float epsilon = Constants::EPSILON_SMALL) {
            return Equals(q, Identity(), epsilon);
        }
    }
}