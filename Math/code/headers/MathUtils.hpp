#pragma once
#include <MathConstants.hpp>
#include <cmath>
#include <algorithm>

namespace MathLib 
{

    // =========================================================
    // Basic comparison and rounding
    // =========================================================

    // Checks if two floats are approximately equal within a tolerance.
    // Direct == comparison on floats is unreliable due to precision errors,
    // so almost all float comparisons in a game engine should use this instead.
    inline bool Approximately(float a, float b, float epsilon = Constants::EPSILON_SMALL) {
        return std::abs(a - b) < epsilon;
    }

    // Checks if a float is approximately zero
    inline bool IsZero(float value, float epsilon = Constants::EPSILON_SMALL) {
        return std::abs(value) < epsilon;
    }

    // Rounds to the nearest integer value, still returned as float
    inline float Round(float value) {
        return std::round(value);
    }

    // Rounds down to the nearest integer value (toward negative infinity)
    inline float Floor(float value) {
        return std::floor(value);
    }

    // Rounds up to the nearest integer value (toward positive infinity)
    inline float Ceil(float value) {
        return std::ceil(value);
    }

    // Truncates the decimal part (rounds toward zero)
    inline float Truncate(float value) {
        return std::trunc(value);
    }

    // Rounds to a specific number of decimal places
    // Example: RoundToDecimals(3.14159f, 2) -> 3.14f
    inline float RoundToDecimals(float value, int decimals) {
        float factor = std::pow(10.0f, static_cast<float>(decimals));
        return std::round(value * factor) / factor;
    }

    // =========================================================
    // Clamping and ranges
    // =========================================================

    // Restricts value to stay within [min, max]
    inline float Clamp(float value, float min, float max) {
        return std::clamp(value, min, max);
    }

    // Restricts value to stay within [0, 1]. Extremely common in graphics
    // (color values, blend factors, interpolation parameters).
    inline float Clamp01(float value) {
        return std::clamp(value, 0.0f, 1.0f);
    }

    // Returns the smaller of two values
    inline float Min(float a, float b) {
        return std::min(a, b);
    }

    // Returns the larger of two values
    inline float Max(float a, float b) {
        return std::max(a, b);
    }

    // Checks if value lies within [min, max] (inclusive)
    inline bool InRange(float value, float min, float max) {
        return value >= min && value <= max;
    }

    // Wraps a value to stay within [min, max), looping around at the edges.
    // Useful for angles (keeping rotation within 0-360) or cyclic UV coordinates.
    inline float Wrap(float value, float min, float max) {
        float range = max - min;
        if (range <= 0.0f) return min;
        float result = std::fmod(value - min, range);
        if (result < 0.0f) result += range;
        return result + min;
    }
    // =========================================================
    // Sign and basic math
    // =========================================================

    // Returns -1, 0, or 1 depending on the sign of the value
    inline float Sign(float value) {
        if (value > 0.0f) return 1.0f;
        if (value < 0.0f) return -1.0f;
        return 0.0f;
    }

    // Returns the absolute value
    inline float Abs(float value) {
        return std::abs(value);
    }

    // Returns value squared (value * value).
    // Prefer this over std::pow(value, 2) - much cheaper, avoids a generic pow call.
    inline float Square(float value) {
        return value * value;
    }

    // Returns value cubed (value * value * value)
    inline float Cube(float value) {
        return value * value * value;
    }

    // Square root
    inline float Sqrt(float value) {
        return std::sqrt(value);
    }

    // Inverse square root (1 / sqrt(value)).
    // Extremely common in graphics (normalizing vectors). GLM/modern compilers
    // already optimize std::sqrt well, so the famous "fast inverse sqrt" trick
    // is mostly historical - this plain version is fine on modern hardware.
    inline float InverseSqrt(float value) {
        return 1.0f / std::sqrt(value);
    }

    // Raises base to the given exponent
    inline float Pow(float base, float exponent) {
        return std::pow(base, exponent);
    }

    // =========================================================
    // Interpolation and remapping
    // =========================================================

    // Linear interpolation: blends between a and b based on t (0 to 1).
    // t=0 returns a, t=1 returns b, values outside [0,1] extrapolate.
    inline float Lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

    // Inverse of Lerp: given a value between a and b, returns what t produced it.
    // Useful for converting a measured value back into a normalized 0-1 range.
    // Example: InverseLerp(0, 100, 25) -> 0.25
    inline float InverseLerp(float a, float b, float value) {
        if (Approximately(a, b)) return 0.0f;
        return (value - a) / (b - a);
    }

    // Remaps a value from one range to another.
    // Example: Remap(50, 0, 100, 0, 1) -> 0.5  (50 in range [0,100] becomes 0.5 in [0,1])
    inline float Remap(float value, float inMin, float inMax, float outMin, float outMax) {
        float t = InverseLerp(inMin, inMax, value);
        return Lerp(outMin, outMax, t);
    }

    // Eased interpolation: same path as Lerp but speed eases in/out
    // (slow-fast-slow) instead of constant speed.
    inline float SmoothStep(float a, float b, float t) {
        float smoothT = Clamp01(t);
        smoothT = smoothT * smoothT * (3.0f - 2.0f * smoothT);
        return Lerp(a, b, smoothT);
    }

    // Even smoother variant of SmoothStep with continuous second derivative
    // (no sudden change in acceleration). Used when SmoothStep still looks
    // too "snappy" at the start/end of the transition.
    inline float SmoothStep2(float a, float b, float t) {
        float smoothT = Clamp01(t);
        smoothT = smoothT * smoothT * smoothT * (smoothT * (smoothT * 6.0f - 15.0f) + 10.0f);
        return Lerp(a, b, smoothT);
    }

    // Moves "current" toward "target" by at most "maxDelta" per call.
    // Frame-rate independent way to approach a value gradually -
    // commonly used for things like smoothly changing a stat, fading volume, etc.
    inline float MoveTowards(float current, float target, float maxDelta) {
        if (std::abs(target - current) <= maxDelta) return target;
        return current + Sign(target - current) * maxDelta;
    }

    // Exponential decay toward target - frame-rate independent smoothing.
    // "speed" controls how fast it converges (higher = faster).
    // More stable than naive Lerp(current, target, speed * dt) across varying frame rates.
    inline float ExpDecay(float current, float target, float speed, float deltaTime) {
        return target + (current - target) * std::exp(-speed * deltaTime);
    }

    

    // =========================================================
    // Trigonometry helpers
    // =========================================================

    inline float Sin(float radians) { return std::sin(radians); }
    inline float Cos(float radians) { return std::cos(radians); }
    inline float Tan(float radians) { return std::tan(radians); }
    inline float Asin(float value) { return std::asin(Clamp(value, -1.0f, 1.0f)); }
    inline float Acos(float value) { return std::acos(Clamp(value, -1.0f, 1.0f)); }
    inline float Atan(float value) { return std::atan(value); }
    inline float Atan2(float y, float x) { return std::atan2(y, x); }

    // Converts degrees to radians
    inline float ToRadians(float degrees) {
        return degrees * Constants::DEG_TO_RAD;
    }

    // Converts radians to degrees
    inline float ToDegrees(float radians) {
        return radians * Constants::RAD_TO_DEG;
    }

    // =========================================================
    // Angle utilities
    // =========================================================

    // Normalizes an angle (in radians) to the range [0, 2*PI)
    inline float NormalizeAngle(float radians) {
        return Wrap(radians, 0.0f, Constants::TWO_PI);
    }

    // Normalizes an angle (in radians) to the range [-PI, PI]
    // Useful for finding the "shortest" representation of a rotation.
    inline float NormalizeAngleSigned(float radians) {
        return Wrap(radians + Constants::PI, 0.0f, Constants::TWO_PI) - Constants::PI;
    }

    // Returns the shortest angular difference between two angles (radians),
    // accounting for wraparound (e.g. the difference between 350° and 10° is 20°, not 340°).
    inline float DeltaAngle(float a, float b) {
        float diff = NormalizeAngleSigned(b - a);
        return diff;
    }

    // Smoothly interpolates between two angles, taking the shortest path
    // around the circle (avoids spinning the "long way" when angles wrap).
    inline float LerpAngle(float a, float b, float t) {
        float delta = DeltaAngle(a, b);
        return a + delta * t;
    }

    // =========================================================
    // Easing functions (common animation curves)
    // =========================================================

    // Quadratic ease-in: starts slow, accelerates
    inline float EaseInQuad(float t) {
        return t * t;
    }

    // Quadratic ease-out: starts fast, decelerates
    inline float EaseOutQuad(float t) {
        return t * (2.0f - t);
    }

    // Quadratic ease-in-out: slow start, fast middle, slow end
    inline float EaseInOutQuad(float t) {
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }

    // Cubic ease-in: more pronounced slow start than quad
    inline float EaseInCubic(float t) {
        return t * t * t;
    }

    // Cubic ease-out: more pronounced slow end than quad
    inline float EaseOutCubic(float t) {
        float f = t - 1.0f;
        return f * f * f + 1.0f;
    }

    // Elastic ease-out: overshoots and "bounces" like a spring settling
    inline float EaseOutElastic(float t) {
        if (IsZero(t)) return 0.0f;
        if (Approximately(t, 1.0f)) return 1.0f;
        float p = 0.3f;
        return std::pow(2.0f, -10.0f * t) * std::sin((t - p / 4.0f) * (Constants::TWO_PI) / p) + 1.0f;
    }

    // Bounce ease-out: simulates a ball bouncing to a stop
    inline float EaseOutBounce(float t) {
        if (t < 1.0f / 2.75f) {
            return 7.5625f * t * t;
        }
        else if (t < 2.0f / 2.75f) {
            t -= 1.5f / 2.75f;
            return 7.5625f * t * t + 0.75f;
        }
        else if (t < 2.5f / 2.75f) {
            t -= 2.25f / 2.75f;
            return 7.5625f * t * t + 0.9375f;
        }
        else {
            t -= 2.625f / 2.75f;
            return 7.5625f * t * t + 0.984375f;
        }
    }

    // =========================================================
    // Useful checks
    // =========================================================

    // Checks if a value is NaN (Not a Number) - typically the result
    // of an invalid operation like 0/0. Useful to catch bugs early.
    inline bool IsNaN(float value) {
        return std::isnan(value);
    }

    // Checks if a value is infinite (positive or negative)
    inline bool IsInfinite(float value) {
        return std::isinf(value);
    }

    // Checks if a value is finite (not NaN, not infinite) - i.e. a "valid" number
    inline bool IsFinite(float value) {
        return std::isfinite(value);
    }

    // Checks if an integer is a power of two (1, 2, 4, 8, 16...).
    // Common check in graphics (texture sizes, buffer alignment).
    inline bool IsPowerOfTwo(unsigned int value) {
        return value != 0 && (value & (value - 1)) == 0;
    }

    // Rounds up to the next power of two.
    // Useful for allocating GPU resources that require power-of-two sizes.
    inline unsigned int NextPowerOfTwo(unsigned int value) {
        if (value == 0) return 1;
        value--;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        return value + 1;
    }

}