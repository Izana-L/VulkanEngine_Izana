#pragma once

#include <Vector.hpp>
#include <cstdint>
namespace ECS
{

    // Light_Component: represents a light source attached to an entity.
    //
    // A single struct with a Type enum is used instead of inheritance because
    // inheritance would break the contiguous array layout that Sparse_Set relies
    // on for cache-efficient iteration — virtual tables add per-element overhead
    // and prevent trivial copying.
    //
    // Position and direction are NOT stored here — they come from the
    // Transform_Component of the same entity:
    //   Directional → Transform.Forward() gives the light direction.
    //   Point       → Transform.position gives the light position.
    //   Spot        → both Transform.position and Transform.Forward().
    //
    // Fields irrelevant to a given Type are ignored by the Extractor:
    //   Directional: color, intensity (range/angles ignored)
    //   Point:       color, intensity, range (angles ignored)
    //   Spot:        color, intensity, range, inner_angle, outer_angle
    struct Light_Component
    {
        enum class Type : uint8_t
        {
            Directional = 0,
            Point = 1,
            Spot = 2,
        };

        // =========================================================
        // Common fields (all light types)
        // =========================================================

        Type             type = Type::Directional;

        // Linear RGB color of the emitted light (not gamma-corrected).
        MathLib::Vector3 color = { 1.0f, 1.0f, 1.0f };

        // Intensity in arbitrary units. The PBR shader multiplies
        // color * intensity to get the final radiance contribution.
        float            intensity = 1.0f;

        // =========================================================
        // Point + Spot only
        // =========================================================

        // Maximum distance at which the light has any effect.
        // Beyond this radius the contribution is zero.
        // Ignored for Directional lights.
        float range = 10.0f;

        // =========================================================
        // Spot only
        // =========================================================

        // Inner cone half-angle in radians. Inside this cone the light
        // is at full intensity.
        float inner_angle = 0.2f;   // ~11.5 degrees

        // Outer cone half-angle in radians. Between inner and outer the
        // light attenuates smoothly (smooth-step falloff).
        // Must be >= inner_angle.
        float outer_angle = 0.4f;   // ~22.9 degrees

        // =========================================================
        // Convenience constructors
        // =========================================================

        static Light_Component Make_directional(const MathLib::Vector3& _color = { 1, 1, 1 },
            float                   _intensity = 1.0f)
        {
            Light_Component light;
            light.type = Type::Directional;
            light.color = _color;
            light.intensity = _intensity;
            return light;
        }

        static Light_Component Make_point(const MathLib::Vector3& _color = { 1, 1, 1 },
            float                   _intensity = 1.0f,
            float                   _range = 10.0f)
        {
            Light_Component light;
            light.type = Type::Point;
            light.color = _color;
            light.intensity = _intensity;
            light.range = _range;
            return light;
        }

        static Light_Component Make_spot(const MathLib::Vector3& _color = { 1, 1, 1 },
            float                   _intensity = 1.0f,
            float                   _range = 10.0f,
            float                   _inner_angle = 0.2f,
            float                   _outer_angle = 0.4f)
        {
            Light_Component light;
            light.type = Type::Spot;
            light.color = _color;
            light.intensity = _intensity;
            light.range = _range;
            light.inner_angle = _inner_angle;
            light.outer_angle = _outer_angle;
            return light;
        }
    };

} // namespace ECS