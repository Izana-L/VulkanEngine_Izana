#pragma once
#include <Vector.hpp>
#include <MathConstants.hpp>
#include <glm/gtx/compatibility.hpp>
#include <cmath>

namespace MathLib 
{
    
    namespace Vec4 
    {
        // 4D vector types
        // Vector4 is most commonly used in 3D graphics to represent:
        //   - Homogeneous coordinates: (x, y, z, w) where w=1 means a point
        //     in 3D space and w=0 means a direction (no translation applied)
        //   - RGBA colors: (r, g, b, a)
        //   - Intermediate results of matrix multiplications (e.g. MVP transform)
        

        // =========================================================
        // Common constants
        // =========================================================
        inline Vector4 Zero()  { return Vector4(0.0f, 0.0f, 0.0f, 0.0f);  }
        inline Vector4 One()   { return Vector4(1.0f, 1.0f, 1.0f, 1.0f);   }
        inline Vector4 UnitX() { return Vector4(1.0f, 0.0f, 0.0f, 0.0f); }
        inline Vector4 UnitY() { return Vector4(0.0f, 1.0f, 0.0f, 0.0f); }
        inline Vector4 UnitZ() { return Vector4(0.0f, 0.0f, 1.0f, 0.0f); }
        inline Vector4 UnitW() { return Vector4(0.0f, 0.0f, 0.0f, 1.0f); }

        // =========================================================
        // Basic operations
        // =========================================================

        // Adds two vectors component-wise: (a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w)
        inline Vector4 Add(const Vector4& a, const Vector4& b) {
            return a + b;
        }

        // Subtracts two vectors component-wise
        inline Vector4 Subtract(const Vector4& a, const Vector4& b) {
            return a - b;
        }

        // Multiplies two vectors component-wise: (a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w)
        // NOTE: NOT the dot product. Just independent per-axis scaling.
        // Commonly used for color modulation: multiplying two RGBA colors component-wise
        // to tint or darken them (e.g. applying a light color to a surface color).
        inline Vector4 Multiply(const Vector4& a, const Vector4& b) {
            return a * b;
        }

        // Divides two vectors component-wise
        inline Vector4 Divide(const Vector4& a, const Vector4& b) {
            return a / b;
        }

        // Scales all components by a single scalar value
        inline Vector4 Scale(const Vector4& v, float scalar) {
            return v * scalar;
        }

        // Flips the sign of all components
        inline Vector4 Negate(const Vector4& v) {
            return -v;
        }

        // =========================================================
        // Dot product and length
        // =========================================================

        // Dot product: a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w -> returns a SCALAR.
        // In 4D, the geometric interpretation is the same as in 2D/3D:
        // measures how aligned two vectors are. However, in practice with homogeneous
        // coordinates (w component), you rarely dot two full Vector4s geometrically —
        // instead, Vector4 dot is most useful when treating the vector as raw data
        // (e.g. dot with a plane equation stored as Vector4 to compute signed distance).
        inline float Dot(const Vector4& a, const Vector4& b) {
            return glm::dot(a, b);
        }

        // NOTE: There is no Cross product for Vector4.
        // Cross product only exists in 3D (and a 7D variant, mathematically).
        // In 4D homogeneous coordinates, cross-product-like operations are done
        // by extracting the xyz components as Vector3 and crossing those instead.

        // Length of the 4D vector: sqrt(x*x + y*y + z*z + w*w)
        // In homogeneous coordinates this rarely represents a meaningful 3D length
        // (you'd usually normalize after perspective divide). More useful for
        // treating Vector4 as a raw 4D direction (e.g. in shaders or neural nets).
        inline float Length(const Vector4& v) {
            return glm::length(v);
        }

        // Length squared: avoids the sqrt() call.
        // Use this for comparisons where the actual length value doesn't matter.
        inline float Length_squared(const Vector4& v) {
            return glm::dot(v, v);
        }

        // Distance between two Vector4 values treated as 4D points
        inline float Distance(const Vector4& a, const Vector4& b) {
            return glm::distance(a, b);
        }

        // Faster distance comparison without sqrt
        inline float Distance_squared(const Vector4& a, const Vector4& b) {
            Vector4 diff = a - b;
            return glm::dot(diff, diff);
        }

        // =========================================================
        // Normalization
        // =========================================================

        // Returns a unit-length Vector4 (all 4 components scaled so total length == 1).
        // In homogeneous coordinates this is NOT the same as the perspective divide
        // (which only divides xyz by w). Use this only when treating Vector4 as a
        // raw 4D direction.
        inline Vector4 Normalize(const Vector4& v) {
            return glm::normalize(v);
        }

        // Checks if the vector already has length ~1 (within a small tolerance)
        inline bool IsNormalized(const Vector4& v, float epsilon = Constants::EPSILON_SMALL) {
            return std::abs(Length_squared(v) - 1.0f) < epsilon;
        }

        // =========================================================
        // Homogeneous coordinate utilities
        // =========================================================

        // Perspective divide: divides x, y, z by w to convert from homogeneous
        // clip space coordinates back to 3D Cartesian coordinates.
        // This is what the GPU does internally after the vertex shader outputs
        // a clip-space Vector4 — it divides by w to get NDC (Normalized Device Coords).
        // You rarely need this manually, but it's useful when reading back GPU results
        // or implementing software rasterization.
        inline Vector4 Perspective_divide(const Vector4& v) {
            if (std::abs(v.w) < 0.0001f) return v; // avoid division by ~zero
            float invW = 1.0f / v.w;
            return Vector4(v.x * invW, v.y * invW, v.z * invW, 1.0f);
        }

        // Extracts only the XYZ components as a 3D vector, discarding W.
        // Use this when you need the 3D position/direction from a homogeneous result.
        inline Vector3 To_vector3(const Vector4& v) {
            return Vector3(v.x, v.y, v.z);
        }

        // Creates a Vector4 from a 3D point: sets w=1 so matrix translations apply.
        // Use this when you want to transform a 3D POINT by a 4x4 matrix.
        inline Vector4 From_point(const glm::vec3& v) {
            return Vector4(v.x, v.y, v.z, 1.0f);
        }

        // Creates a Vector4 from a 3D direction: sets w=0 so matrix translations
        // do NOT apply (only rotation and scale affect directions, not translation).
        // Use this when you want to transform a 3D DIRECTION by a 4x4 matrix.
        inline Vector4 From_direction(const glm::vec3& v) {
            return Vector4(v.x, v.y, v.z, 0.0f);
        }

        // =========================================================
        // Color utilities (when using Vector4 as RGBA)
        // =========================================================

        // Clamps all components to [0, 1], ensuring a valid RGBA color range
        inline Vector4 Clamp_color(const Vector4& color) {
            return glm::clamp(color, 0.0f, 1.0f);
        }

        // Converts a linear color to gamma-corrected sRGB space (gamma ~2.2).
        // Linear colors are used in lighting math; sRGB is what monitors display.
        // You typically do this as the very last step before outputting a pixel.
        inline Vector4 Linear_to_gamma(const Vector4& color) {
            return Vector4(
                std::pow(color.r, Constants::INV_GAMMA),
                std::pow(color.g, Constants::INV_GAMMA),
                std::pow(color.b, Constants::INV_GAMMA),
                color.a  // alpha is not gamma corrected
            );
        }

        // Converts an sRGB color back to linear space.
        // You should do this when READING a texture that was stored in sRGB,
        // before using it in any lighting calculations.
        inline Vector4 Gamma_to_linear(const Vector4& color) 
        {
            return Vector4(
                std::pow(color.r, Constants::GAMMA),
                std::pow(color.g, Constants::GAMMA),
                std::pow(color.b, Constants::GAMMA),
                color.a  // alpha is not gamma corrected
            );
        }

        // =========================================================
        // Interpolation
        // =========================================================

        // Linear interpolation between a and b. Straight-line blend, constant speed.
        // t=0 returns a, t=1 returns b.
        // Commonly used for blending RGBA colors (e.g. fading between two color values).
        inline Vector4 Lerp(const Vector4& a, const Vector4& b, float t) {
            return glm::mix(a, b, t);
        }

        // Spherical interpolation: blends along an arc at constant angular speed.
        // Less common for Vector4 than for quaternions or Vector3 directions,
        // but useful when the w component also needs to be interpolated smoothly
        // along with the direction (e.g. some shader parameter blending).
        inline Vector4 Slerp(const Vector4& a, const Vector4& b, float t) {
            float lenA = Length(a);
            float lenB = Length(b);
            Vector4 normA = Normalize(a);
            Vector4 normB = Normalize(b);

            float dot = glm::clamp(Dot(normA, normB), -1.0f, 1.0f);
            float theta = std::acos(dot) * t;

            Vector4 relative = Normalize(normB - normA * dot);
            Vector4 dir = normA * std::cos(theta) + relative * std::sin(theta);

            float len = glm::mix(lenA, lenB, t);
            return dir * len;
        }

        // Eased interpolation: same path as Lerp but with smooth ease-in/ease-out.
        // Useful for color transitions and UI animations that need to feel less abrupt.
        inline Vector4 Smooth_step(const Vector4& a, const Vector4& b, float t) {
            float smoothT = t * t * (3.0f - 2.0f * t);
            return Lerp(a, b, smoothT);
        }

        // =========================================================
        // Reflection and projection
        // =========================================================

      

        // Reflects v off a hyperplane defined by a 4D normal
        // (rare in practice; most reflections should be done in 3D with Vector3)
        inline Vector4 Reflect(const Vector4& v, const Vector4& normal) {
            return glm::reflect(v, normal);
        }

        // Projects v onto the direction of "onto"
        inline Vector4 Project(const Vector4& v, const Vector4& onto) {
            float ontoLenSq = Length_squared(onto);
            if (ontoLenSq < Constants::EPSILON_SMALL) return Zero();
            return onto * (Dot(v, onto) / ontoLenSq);
        }

        // Returns the component of v perpendicular to "onto". Project + Reject = v.
        inline Vector4 Reject(const Vector4& v, const Vector4& onto) {
            return v - Project(v, onto);
        }

        // =========================================================
        // Comparison and utility functions
        // =========================================================

        // Checks if two Vector4 values are approximately equal (within epsilon)
        inline bool Equals(const Vector4& a, const Vector4& b, float epsilon = Constants::EPSILON_SMALL) {
            return Distance_squared(a, b) < (epsilon * epsilon);
        }

        // Returns the smaller value of each component independently
        inline Vector4 Min(const Vector4& a, const Vector4& b) {
            return glm::min(a, b);
        }

        // Returns the larger value of each component independently
        inline Vector4 Max(const Vector4& a, const Vector4& b) {
            return glm::max(a, b);
        }

        // Clamps each component of v to stay within [min, max]
        inline Vector4 Clamp(const Vector4& v, const Vector4& min, const Vector4& max) {
            return glm::clamp(v, min, max);
        }

        // Returns the absolute value of each component
        inline Vector4 Abs(const Vector4& v) {
            return glm::abs(v);
        }

        // Returns the sign of each component (-1, 0, or 1)
        inline Vector4 Sign(const Vector4& v) {
            return glm::sign(v);
        }


    }
}