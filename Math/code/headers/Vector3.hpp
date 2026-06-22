#pragma once
#include <Vector.hpp>
#include <MathConstants.hpp>
#include <glm/gtx/compatibility.hpp>
#include <cmath>

namespace MathLib 
{
    
    namespace Vec3 
    {
        // 3D vector types
       

        // =========================================================
        // Common constants
        // =========================================================

        inline Vector3 Zero() { return Vector3(0.0f, 0.0f, 0.0f); }
        inline Vector3 One() { return Vector3(1.0f, 1.0f, 1.0f); }
        inline Vector3 UnitX() { return Vector3(1.0f, 0.0f, 0.0f); }
        inline Vector3 UnitY() { return Vector3(0.0f, 1.0f, 0.0f); }
        inline Vector3 UnitZ() { return Vector3(0.0f, 0.0f, 1.0f); }
        inline Vector3 PlaneXY() { return Vector3(1.0f, 1.0f, 0.0f); }
        inline Vector3 PlaneXZ() { return Vector3(1.0f, 0.0f, 1.0f); }
        inline Vector3 PlaneYZ() { return Vector3(0.0f, 1.0f, 1.0f); }

        // =========================================================
        // Basic operations
        // =========================================================

        // Adds two vectors component-wise
        inline Vector3 Add(const Vector3& a, const Vector3& b) {
            return a + b;
        }

        // Subtracts two vectors component-wise
        inline Vector3 Subtract(const Vector3& a, const Vector3& b) {
            return a - b;
        }

        // Multiplies two vectors component-wise: (a.x*b.x, a.y*b.y, a.z*b.z)
        // NOTE: this is NOT the dot product (see Dot below). It just scales
        // each axis independently and still returns a vector, not a scalar.
        inline Vector3 Multiply(const Vector3& a, const Vector3& b) {
            return a * b;
        }

        // Divides two vectors component-wise
        inline Vector3 Divide(const Vector3& a, const Vector3& b) {
            return a / b;
        }

        // Scales a vector uniformly by a single scalar
        inline Vector3 Scale(const Vector3& v, float scalar) {
            return v * scalar;
        }

        // Flips the direction of a vector
        inline Vector3 Negate(const Vector3& v) {
            return -v;
        }

        // =========================================================
        // Dot product, cross product, length
        // =========================================================

        // Dot product: a.x*b.x + a.y*b.y + a.z*b.z -> returns a SCALAR.
        // Measures how aligned two vectors are:
        //   - Positive -> similar direction (angle < 90°)
        //   - Zero     -> perpendicular
        //   - Negative -> opposite direction (angle > 90°)
        // Used for: lighting (N dot L), checking if something faces the camera,
        // projections, angle calculations.
        inline float Dot(const Vector3& a, const Vector3& b) {
            return glm::dot(a, b);
        }

        // Cross product: returns a VECTOR perpendicular to BOTH a and b,
        // following the right-hand rule. This only exists meaningfully in 3D
        // (unlike the 2D version, which returns a scalar since there's no
        // "third dimension" to point into).
        // The magnitude of the result equals |a|*|b|*sin(angle between them).
        // Used for: computing surface normals from two edges of a triangle,
        // building orthogonal basis vectors (e.g. camera right/up vectors).
        inline Vector3 Cross(const Vector3& a, const Vector3& b) {
            return glm::cross(a, b);
        }

        // Length (magnitude): sqrt(x*x + y*y + z*z)
        inline float Length(const Vector3& v) {
            return glm::length(v);
        }

        // Length squared: skips the sqrt() call.
        // Prefer this over Length() when only comparing distances,
        // since sqrt is relatively expensive.
        inline float Length_squared(const Vector3& v) {
            return glm::dot(v, v);
        }

        // Distance between two points in 3D space
        inline float Distance(const Vector3& a, const Vector3& b) {
            return glm::distance(a, b);
        }

        // Faster distance comparison without sqrt
        inline float Distance_squared(const Vector3& a, const Vector3& b) {
            Vector3 diff = a - b;
            return glm::dot(diff, diff);
        }

        // =========================================================
        // Normalization
        // =========================================================

        // Returns a vector with the same direction but length 1.
        // Critical in 3D graphics: normals, light directions, camera directions
        // all need to be unit-length for lighting math to be correct.
        inline Vector3 Normalize(const Vector3& v) {
            return glm::normalize(v);
        }

        // Checks if a vector already has length ~1 (within a small tolerance)
        inline bool Is_normalized(const Vector3& v, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(Length_squared(v) - 1.0f) < epsilon;
        }

        // =========================================================
        // Angles and rotation
        // =========================================================

        // Returns the angle (in radians) between two vectors, ignoring their length.
        // Unlike 2D, there's no single "Angle()" function relative to one axis here,
        // since in 3D a vector's orientation can't be described by one angle alone.
        inline float Angle_between(const Vector3& a, const Vector3& b) {
            float dot = Dot(Normalize(a), Normalize(b));
            dot = glm::clamp(dot, -1.0f, 1.0f); // avoid NaN from floating point errors in acos
            return std::acos(dot);
        }

        // Rotates vector v around an arbitrary axis by the given angle (radians),
        // using Rodrigues' rotation formula:
        //   v_rot = v*cos(theta) + (axis × v)*sin(theta) + axis*(axis · v)*(1-cos(theta))
        // This lets you rotate around ANY axis, not just X/Y/Z - useful when you
        // don't want to build a full rotation matrix or quaternion for a single rotation.
        inline Vector3 Rotate_around_axis(const Vector3& v, const Vector3& axis, float radians) {
            Vector3 normAxis = Normalize(axis); // the formula assumes a unit-length axis
            float c = std::cos(radians);
            float s = std::sin(radians);
            return v * c + Cross(normAxis, v) * s + normAxis * Dot(normAxis, v) * (1.0f - c);
        }

        // =========================================================
        // Interpolation
        // =========================================================

        // Linear interpolation: straight-line blend between a and b, constant speed.
        // t=0 returns a, t=1 returns b.
        inline Vector3 Lerp(const Vector3& a, const Vector3& b, float t) {
            return glm::mix(a, b, t);
        }

        // Spherical interpolation: instead of cutting straight across like Lerp,
        // this moves ALONG THE ARC between the two directions, at constant angular
        // speed, while separately blending their lengths.
        // Use this when interpolating DIRECTIONS (e.g. smoothly turning a camera
        // or character from facing one way to facing another) rather than positions,
        // since Lerp-ing two directions can shrink/distort the path in between.
        inline Vector3 Slerp(const Vector3& a, const Vector3& b, float t)
        {
            Vector3 normA = Normalize(a);
            Vector3 normB = Normalize(b);

            float dot = glm::clamp(Dot(normA, normB), -1.0f, 1.0f);
            float theta = std::acos(dot) * t; // angle to rotate by, scaled by t

            // build a vector perpendicular to normA, lying in the plane of normA/normB
            Vector3 relative = Normalize(normB - normA * dot);
            Vector3 dir = normA * std::cos(theta) + relative * std::sin(theta);

            float len = glm::mix(Length(a), Length(b), t);
            return dir * len;
        }

        // Eased interpolation: same straight-line path as Lerp, but speed eases
        // in and out (slow-fast-slow) instead of being constant.
        // Good for animations/transitions that should feel less mechanical.
        inline Vector3 SmoothStep(const Vector3& a, const Vector3& b, float t) {
            float smoothT = t * t * (3.0f - 2.0f * t); // classic smoothstep curve
            return Lerp(a, b, smoothT);
        }

        // =========================================================
        // Reflection and projection
        // =========================================================



        // Reflects vector v off a surface with the given normal
        // (e.g. a ball bouncing off a wall, light bouncing off a mirror)
        inline Vector3 Reflect(const Vector3& v, const Vector3& normal) {
            return glm::reflect(v, normal);
        }

        // Projects v onto the direction of "onto": returns the component of v
        // that points in the same direction as "onto".
        // Used for: sliding movement along a surface, shadow/lighting math.
        inline Vector3 Project(const Vector3& v, const Vector3& onto) {
            float ontoLenSq = Length_squared(onto);
            if (ontoLenSq < Constants::EPSILON_SMALL) return Zero(); // avoid division by ~zero
            return onto * (Dot(v, onto) / ontoLenSq);
        }

        // Returns the component of v that is PERPENDICULAR to "onto"
        // (the part Project() does NOT capture). Project + Reject = v.
        inline Vector3 Reject(const Vector3& v, const Vector3& onto) {
            return v - Project(v, onto);
        }

        // Projects v onto a plane defined by its normal - useful for things like
        // making a character "slide" along the ground instead of moving into it.
        // This is just an alias for Reject(), since removing the component along
        // the normal is exactly what "projecting onto the plane" means.
        inline Vector3 Project_on_plane(const Vector3& v, const Vector3& planeNormal) {
            return Reject(v, planeNormal);
        }

        // =========================================================
        // Comparison and utility functions
        // =========================================================

        // Checks if two vectors are approximately equal (within epsilon),
        // since floating point values are rarely EXACTLY equal
        inline bool Equals(const Vector3& a, const Vector3& b, float epsilon = Constants::EPSILON_SMALL) 
        {
            return Distance_squared(a, b) < (epsilon * epsilon);
        }

        // Returns the smaller value of each component independently
        inline Vector3 Min(const Vector3& a, const Vector3& b) {
            return glm::min(a, b);
        }

        // Returns the larger value of each component independently
        inline Vector3 Max(const Vector3& a, const Vector3& b) {
            return glm::max(a, b);
        }

        // Clamps each component of v to stay within [min, max]
        inline Vector3 Clamp(const Vector3& v, const Vector3& min, const Vector3& max) {
            return glm::clamp(v, min, max);
        }

        // Returns the absolute value of each component
        inline Vector3 Abs(const Vector3& v) {
            return glm::abs(v);
        }

        // Returns the sign of each component (-1, 0, or 1)
        inline Vector3 Sign(const Vector3& v) {
            return glm::sign(v);
        }


    }
}