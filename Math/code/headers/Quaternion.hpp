#pragma once
#include <GlmConfig.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <MathConstants.hpp>
#include <cmath>
#include <Vector.hpp>
#include <Vector3.hpp>
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
        // The axis does NOT need to be normalized beforehand; it is normalized here.
        // A zero-length axis defines no rotation, so the identity is returned.
        // This is the most common way to build a rotation procedurally
        // (e.g. "rotate 90 degrees around the Y axis").
        inline Quaternion From_axis_angle(const Vector3& axis, float radians) {
            const Vector3 unitAxis = Vec3::Normalize(axis);
            if (Vec3::Length_squared(unitAxis) == 0.0f) return Identity();
            return glm::angleAxis(radians, unitAxis);
        }

        // Builds a quaternion from Euler angles (pitch, yaw, roll), in radians.
        // Useful for exposing rotation in an editor UI, since Euler angles are
        // more intuitive for humans than raw quaternion components.
        // Applied to a vector the rotations happen in the order X (pitch) first,
        // then Y (yaw), then Z (roll), around the FIXED world axes:
        //   R = Rz * Ry * Rx
        // Mat4::Rotation_euler uses the same convention.
        // NOTE: convert back and forth carries the usual Euler angle quirks
        // (see ToEuler: the gimbal lock of this convention sits at yaw = +-90 degrees).
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
        // A zero-length vector has no direction, so the identity is returned.
        //
        // Built as the quaternion (1 + f.t, f x t), normalized: that is the
        // half-angle form of the axis-angle quaternion, so it needs neither acos
        // nor a normalized cross product and stays exact for tiny angles (the
        // usual "if (dot ~ 1) return identity" shortcut silently drops any rotation
        // under a degree). Only exactly opposite vectors need special care.
        inline Quaternion From_to_rotation(const Vector3& from, const Vector3& to) {
            const Vector3 f = Vec3::Normalize(from);
            const Vector3 t = Vec3::Normalize(to);
            if (Vec3::Length_squared(f) == 0.0f || Vec3::Length_squared(t) == 0.0f) {
                return Identity();
            }

            // Everything is derived from sum = f + t, which is small (and exact) exactly
            // when the vectors are nearly opposite, the case where "1 + dot" and
            // cross(f, t) are swamped by rounding noise:
            //   1 + f.t     == |sum|^2 / 2   (unit vectors)
            //   cross(f, t) == cross(f, sum) (f x f == 0)
            const Vector3 sum = f + t;
            const float sum_sq = glm::dot(sum, sum);

            // vectors point in opposite directions: half a turn around any perpendicular axis
            if (sum_sq <= Constants::EPSILON * Constants::EPSILON) {
                return glm::angleAxis(Constants::PI, Vec3::Any_perpendicular(f));
            }

            return glm::normalize(Quaternion(0.5f * sum_sq, glm::cross(f, sum))); // (w, xyz)
        }

        // Builds a quaternion that orients an object to look toward "forward",
        // with "up" defining the roll. Equivalent to a rotation-only LookAt.
        // Useful for making an object face a target (e.g. a turret aiming at a player).
        // Never produces NaN: a zero-length forward returns the identity, and an up
        // that is zero or parallel to forward is replaced by an arbitrary
        // perpendicular of forward (see Vec3::Safe_up).
        inline Quaternion Look_rotation(const Vector3& forward, const Vector3& up = Vec3::Up())
        {
            const Vector3 f = Vec3::Normalize(forward);
            if (Vec3::Length_squared(f) == 0.0f) return Identity();
            return glm::quatLookAt(f, Vec3::Safe_up(f, up));
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
        // map to different Euler angle results depending on the order/convention.
        // With this module's convention (R = Rz * Ry * Rx, see From_euler) pitch and
        // roll come back through atan2 (full +-PI range) but yaw comes back through
        // asin, so it is only ever returned in [-PI/2, PI/2]:
        //   - a rotation with |yaw| > 90 degrees comes back as an EQUIVALENT triple
        //     (pitch + PI, PI - yaw, roll + PI), so never rebuild a rotation from
        //     only some of the three angles;
        //   - at yaw = +-90 degrees (gimbal lock) pitch and roll rotate around the
        //     same axis and only their difference/sum is recoverable.
        // To read a camera's yaw/pitch, derive them from GetForward/GetRight instead.
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

        // Linear interpolation between two quaternions, then re-normalizes
        // ("nlerp"). Cheaper than Slerp but does NOT produce constant angular
        // speed - the rotation speeds up/slows down non-uniformly across t.
        // Good enough for small angle differences or non-critical blending,
        // and significantly cheaper to compute than Slerp.
        // Takes the SHORTEST path: q and -q are the same rotation, so when the two
        // quaternions lie in opposite hemispheres (dot < 0) b is negated first.
        // Without that the blend would go the long way around.
        // (Not glm::mix: for quaternions that is a slerp, not a lerp.)
        inline Quaternion Lerp(const Quaternion& a, const Quaternion& b, float t) {
            const Quaternion target = (glm::dot(a, b) < 0.0f) ? -b : b;
            return glm::normalize(a * (1.0f - t) + target * t);
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

        // Returns the angle (in radians, in [0, PI]) needed to rotate from quaternion a to b.
        // Useful for checking how "different" two rotations are
        // (e.g. snapping logic, rotation speed limiting).
        // Computed from the relative rotation conj(a) * b as 2 * atan2(|xyz|, |w|)
        // rather than 2 * acos(|dot|): acos is ill-conditioned near 1 (the dot of
        // two rotations under ~0.04 degrees apart rounds to exactly 1 and reads as
        // "identical"), while atan2 keeps its precision at every angle.
        // |w| makes q and -q the same rotation, and the result does not depend on
        // the quaternions' lengths.
        inline float Angle_between(const Quaternion& a, const Quaternion& b) {
            const Quaternion relative = glm::conjugate(a) * b;
            const float vector_length = glm::length(Vector3(relative.x, relative.y, relative.z));
            return 2.0f * std::atan2(vector_length, std::abs(relative.w));
        }

        // =========================================================
        // Direction vectors derived from a rotation
        // =========================================================

        // Returns the local "forward" direction after applying this rotation.
        // Assumes -Z is forward in local space (common convention; verify against
        // your engine's coordinate system, some use +Z as forward instead).
        inline Vector3 GetForward(const Quaternion& q) {
            return Rotate_vector(q, Vec3::Forward());
        }

        // Returns the local "right" direction after applying this rotation
        inline Vector3 GetRight(const Quaternion& q) {
            return Rotate_vector(q, Vec3::Right());
        }

        // Returns the local "up" direction after applying this rotation
        inline Vector3 GetUp(const Quaternion& q) {
            return Rotate_vector(q, Vec3::Up());
        }

        // =========================================================
        // Comparison and utilities
        // =========================================================

        // Checks if two quaternions are approximately equal: they represent
        // rotations less than "epsilon" RADIANS apart.
        // NOTE: a quaternion Q and its negation -Q represent the SAME rotation,
        // and Angle_between already treats them as such.
        // The tolerance is an ANGLE on purpose. Comparing |dot| against 1 would make
        // epsilon act on the SQUARE of the angle (1 - cos(theta/2) ~ theta^2/8), so
        // the default 1e-4 would accept rotations about 1.6 degrees apart.
        inline bool Equals(const Quaternion& a, const Quaternion& b, float epsilon = Constants::EPSILON_SMALL) {
            return Angle_between(a, b) < epsilon;
        }

        // Checks if the quaternion is approximately the identity (no rotation):
        // its rotation angle is below "epsilon" radians.
        inline bool Is_identity(const Quaternion& q, float epsilon = Constants::EPSILON_SMALL) {
            return Equals(q, Identity(), epsilon);
        }
    }
}