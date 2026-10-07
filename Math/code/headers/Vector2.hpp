#pragma once

#include <Vector.hpp>
#include <MathConstants.hpp>
#include <glm/gtx/compatibility.hpp>
#include <cmath>

namespace MathLib 
{
    
    namespace Vec2 
    {
        // 2D vector types
       

        // =========================================================
        // Common constants
        // =========================================================
        inline Vector2 Zero()  { return Vector2(0.0f, 0.0f); }
        inline Vector2 One()   { return Vector2(1.0f, 1.0f); }
        inline Vector2 UnitX() { return Vector2(1.0f, 0.0f); }
        inline Vector2 UnitY() { return Vector2(0.0f, 1.0f); }

        // =========================================================
        // Basic operations
        // =========================================================

        // Adds two vectors component-wise: (a.x+b.x, a.y+b.y)
        inline Vector2 Add(const Vector2& a, const Vector2& b)
        {
            return a + b;
        }

        // Subtracts two vectors component-wise: (a.x-b.x, a.y-b.y)
        inline Vector2 Subtract(const Vector2& a, const Vector2& b)
        {
            return a - b;
        }

        // Multiplies two vectors component-wise: (a.x*b.x, a.y*b.y)
        // NOTE: this is NOT the dot product. See the comparison below.
        inline Vector2 Multiply(const Vector2& a, const Vector2& b)
        {
            return a * b;
        }

        // Divides two vectors component-wise: (a.x/b.x, a.y/b.y)
        inline Vector2 Divide(const Vector2& a, const Vector2& b)
        {
            return a / b;
        }

        // Scales a vector by a single scalar value (uniform scale)
        inline Vector2 Scale(const Vector2& v, float scalar)
        {
            return v * scalar;
        }

        // Flips the direction of a vector (points the opposite way)
        inline Vector2 Negate(const Vector2& v)
        {
            return -v;
        }

        // =========================================================
        // Dot product, cross product, length
        // =========================================================

        // Dot product: a.x*b.x + a.y*b.y -> returns a SCALAR, not a vector.
        // Geometrically: measures how much two vectors point in the same direction.
        //   - Positive  -> vectors point in a similar direction (angle < 90°)
        //   - Zero      -> vectors are perpendicular
        //   - Negative  -> vectors point in roughly opposite directions (angle > 90°)
        // Used for: lighting calculations, checking facing direction, projections.
        inline float Dot(const Vector2& a, const Vector2& b)
        {
            return glm::dot(a, b);
        }

        // 2D "cross product": there is no real cross product in 2D (that would need
        // a 3rd dimension), so this returns the scalar equivalent of the Z component
        // you'd get if you cross two 3D vectors with Z=0.
        // Useful to know the rotation direction (clockwise/counter-clockwise)
        // or which side of a line a point is on.
        inline float Cross(const Vector2& a, const Vector2& b)
        {
            return a.x * b.y - a.y * b.x;
        }

        // Length (magnitude) of the vector: sqrt(x*x + y*y)
        inline float Length(const Vector2& v)
        {
            return glm::length(v);
        }

        // Length squared: skips the sqrt() call.
        // Use this instead of Length() whenever you only need to COMPARE distances
        // (e.g. "is A closer than B?"), since sqrt is relatively expensive.
        inline float Length_squared(const Vector2& v)
        {
            return glm::dot(v, v);
        }

        // Distance between two points (treats both vectors as positions)
        inline float Distance(const Vector2& a, const Vector2& b)
        {
            return glm::distance(a, b);
        }

        // Same idea as LengthSquared: faster distance comparison without sqrt
        inline float Distance_squared(const Vector2& a, const Vector2& b)
        {
            return glm::dot(a - b, a - b);
        }

        // =========================================================
        // Normalization
        // =========================================================

        // Returns a vector with the same direction but length 1.
        // Used everywhere direction matters but magnitude doesn't (e.g. surface normals).
        // A zero-length vector has no direction, so it is returned as ZERO
        // instead of NaN (glm::normalize divides 0 by 0). There is no absolute
        // "small" threshold: only a squared length below FLOAT_MIN (|v| < ~1e-19,
        // where it underflows) counts as zero. NaN input still gives NaN.
        inline Vector2 Normalize(const Vector2& v)
        {
            const float len_sq = Length_squared(v);
            return (len_sq <= Constants::FLOAT_MIN) ? Zero() : v * (1.0f / std::sqrt(len_sq));
        }

        // Checks if a vector already has length ~1 (within a small tolerance)
        inline bool Is_normalized(const Vector2& v, float epsilon = Constants::EPSILON_SMALL)
        {
            return std::abs(Length_squared(v) - 1.0f) < epsilon;
        }

        // =========================================================
        // Angles and rotation
        // =========================================================

        // Returns the angle (in radians) of the vector relative to the positive X axis.
        // Example: Angle(Vector2(1,0)) == 0, Angle(Vector2(0,1)) == PI/2
        inline float Angle(const Vector2& v)
        {
            return std::atan2(v.y, v.x);
        }

        // Creates a unit-length vector pointing at the given angle (in radians)
        // Example: FromAngle(0) == (1,0), FromAngle(PI/2) == (0,1)
        inline Vector2 From_angle(float radians) {
            return Vector2(std::cos(radians), std::sin(radians));
        }

        // Returns the unsigned angle (in radians, in [0, PI]) between two vectors,
        // ignoring their length.
        // Computed as atan2(|a x b|, a . b) rather than acos(a . b): acos is
        // ill-conditioned near 0 and PI, while atan2 keeps its precision at every
        // angle. It also needs no normalization, so a zero-length vector gives 0
        // instead of NaN.
        inline float Angle_between(const Vector2& a, const Vector2& b)
        {
            return std::atan2(std::abs(Cross(a, b)), Dot(a, b));
        }

        // Rotates a vector by the given angle (in radians), counter-clockwise
        inline Vector2 Rotate(const Vector2& v, float radians)
        {
            float c = std::cos(radians);
            float s = std::sin(radians);
            return Vector2(v.x * c - v.y * s, v.x * s + v.y * c);
        }

        // Returns a vector perpendicular to v (rotated 90° counter-clockwise)
        inline Vector2 Perpendicular(const Vector2& v)
        {
            return Vector2(-v.y, v.x);
        }

        // =========================================================
        // Interpolation
        // =========================================================

        // --- Lerp vs Slerp vs SmoothStep ---
        //
        // Lerp (Linear interpolation):
        //   Moves in a STRAIGHT LINE from a to b. Both position AND speed change
        //   linearly with t. Good for: moving an object directly from point A to B.
        //   Problem: if a and b are meant to represent DIRECTIONS, lerping them
        //   produces a path that doesn't preserve length/rotation smoothly
        //   (it cuts across, rather than curving).
        //
        // Slerp (Spherical interpolation):
        //   Interpolates ALONG THE ARC instead of a straight line - it blends the
        //   angle and length separately, producing smooth ROTATIONAL movement.
        //   Good for: interpolating directions/rotations (e.g. smoothly turning
        //   a 2D character or camera from facing one way to facing another).
        //   In 2D this matters less than in 3D/quaternions, but it's still useful
        //   when interpolating directional vectors rather than positions.
        //
        // SmoothStep:
        //   Still moves in a straight line like Lerp (same path), but eases the
        //   SPEED in and out: starts slow, speeds up in the middle, slows down
        //   at the end. Good for: animations that should feel less mechanical
        //   than linear motion (UI transitions, camera easing).
        //
        // Summary:
        //   Lerp        -> straight line, constant speed
        //   Slerp       -> curved path, constant angular speed (for directions)
        //   SmoothStep  -> straight line, eased speed (for nicer-feeling motion)

        // Linear interpolation between a and b. t=0 returns a, t=1 returns b.
        inline Vector2 Lerp(const Vector2& a, const Vector2& b, float t)
        {
            return glm::mix(a, b, t);
        }

        // Spherical interpolation: blends angle and length separately so the
        // result curves smoothly between directions instead of cutting straight across.
        // The angle travelled is the SIGNED SHORTEST one between the two directions,
        // so it never takes the long way around (a direction just below +PI and one
        // just above -PI are close neighbours, not 340 degrees apart).
        // A zero-length vector has no direction, so it falls back to Lerp.
        inline Vector2 Slerp(const Vector2& a, const Vector2& b, float t)
        {
            const float lenA = Length(a);
            if (Length_squared(a) <= Constants::FLOAT_MIN || Length_squared(b) <= Constants::FLOAT_MIN)
                return Lerp(a, b, t);

            const float len = glm::mix(lenA, Length(b), t);
            const float delta = std::atan2(Cross(a, b), Dot(a, b)); // signed shortest angle, in (-PI, PI]

            return Rotate(a / lenA, delta * t) * len;
        }

        // Eased interpolation: same path as Lerp, but with a smooth acceleration
        // and deceleration curve instead of constant speed.
        // t is clamped to [0, 1], like GLSL smoothstep: the curve overshoots
        // wildly outside that range (t=2 would give -4).
        inline Vector2 Smooth_step(const Vector2& a, const Vector2& b, float t)
        {
            float clampedT = glm::clamp(t, 0.0f, 1.0f);
            float smoothT = clampedT * clampedT * (3.0f - 2.0f * clampedT); // classic smoothstep curve
            return Lerp(a, b, smoothT);
        }

        // =========================================================
        // Reflection and projection
        // =========================================================

        // Reflects vector v off a surface with the given normal
        // (like a ball bouncing off a wall)
        inline Vector2 Reflect(const Vector2& v, const Vector2& normal)
        {
            return glm::reflect(v, normal);
        }

        // Returns a vector of zero length, used as a safe fallback value


        // Projects v onto the direction of "onto": returns the component of v
        // that points in the same direction as "onto".
        // Used for: sliding movement along a surface, shadow calculations.
        // The result does not depend on the length of "onto", so there is no
        // "too short" threshold: only a (numerically) zero-length "onto", which
        // has no direction to project on, returns zero.
        inline Vector2 Project(const Vector2& v, const Vector2& onto)
        {
            float ontoLenSq = Length_squared(onto);
            if (ontoLenSq <= Constants::FLOAT_MIN) return Zero(); // avoid division by zero
            return onto * (Dot(v, onto) / ontoLenSq);
        }

        // Returns the component of v that is PERPENDICULAR to "onto"
        // (the part Project() does NOT capture). Project + Reject = v.
        inline Vector2 Reject(const Vector2& v, const Vector2& onto)
        {
            return v - Project(v, onto);
        }

        // =========================================================
        // Comparison and utility functions
        // =========================================================

        // Checks if two vectors are approximately equal (within epsilon),
        // useful since floating point values are rarely EXACTLY equal
        inline bool Equals(const Vector2& a, const Vector2& b, float epsilon = Constants::EPSILON_SMALL)
        {
            return Distance_squared(a, b) < (epsilon * epsilon);
        }

        // Returns the smaller value of each component independently
        inline Vector2 Min(const Vector2& a, const Vector2& b)
        {
            return glm::min(a, b);
        }

        // Returns the larger value of each component independently
        inline Vector2 Max(const Vector2& a, const Vector2& b)
        {
            return glm::max(a, b);
        }

        // Clamps each component of v to stay within [min, max]
        inline Vector2 Clamp(const Vector2& v, const Vector2& min, const Vector2& max)
        {
            return glm::clamp(v, min, max);
        }

        // Returns the absolute value of each component
        inline Vector2 Abs(const Vector2& v)
        {
            return glm::abs(v);
        }

        // Returns the sign of each component (-1, 0, or 1)
        inline Vector2 Sign(const Vector2& v)
        {
            return glm::sign(v);
        }

    }

}