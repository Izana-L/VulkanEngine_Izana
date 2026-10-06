#pragma once

#include <Vector.hpp>
#include <MathConstants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace ECS
{

    // Light_Component: represents a light source attached to an entity.
    //
    // A single struct with a Type enum is used instead of inheritance because
    // inheritance would break the contiguous array layout that Component_Storage relies
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
        MathLib::Vector3 color{ 1.0f, 1.0f, 1.0f };

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
        // Must be STRICTLY greater than inner_angle: the shader evaluates
        // smoothstep(cos(outer), cos(inner), x), which GLSL leaves undefined
        // when the two edges are equal or reversed.
        float outer_angle = 0.4f;   // ~22.9 degrees

        // =========================================================
        // Validation
        // =========================================================

        // Widest cone half-angle accepted (a hemisphere).
        static constexpr float MAX_CONE_ANGLE = MathLib::Constants::HALF_PI;

        // Smallest gap kept between inner and outer by Sanitized().
        static constexpr float MIN_CONE_GAP = MathLib::Constants::EPSILON_SMALL;

        // True when every field is finite and in range for this light's type:
        // color and intensity >= 0 for all; range > 0 for Point and Spot;
        // 0 <= inner_angle < outer_angle <= MAX_CONE_ANGLE for Spot.
        bool Is_valid() const noexcept
        {
            return Invalid_field() == nullptr;
        }

        // Name of the first invalid field, or nullptr when the light is valid.
        // The factories use it to say WHAT was wrong.
        const char* Invalid_field() const noexcept
        {
            const auto finite_non_negative = [](float v) { return std::isfinite(v) && v >= 0.0f; };

            if (!finite_non_negative(color.x) || !finite_non_negative(color.y) || !finite_non_negative(color.z))
                return "color";
            if (!finite_non_negative(intensity)) return "intensity";

            if (type == Type::Directional) return nullptr;

            if (!std::isfinite(range) || range <= 0.0f) return "range";

            if (type == Type::Spot)
            {
                if (!std::isfinite(inner_angle) || inner_angle < 0.0f) return "inner_angle";
                if (!std::isfinite(outer_angle) || outer_angle > MAX_CONE_ANGLE) return "outer_angle";
                if (!(inner_angle < outer_angle)) return "inner_angle (must be < outer_angle)";
            }

            return nullptr;
        }

        // Returns a copy that is always safe to hand to the GPU: non-finite or
        // negative values fall back to the defaults, and a spot cone is forced
        // into 0 <= inner < outer <= MAX_CONE_ANGLE. The fields are public and
        // can change after construction, so the code that fills the RenderPacket
        // uses this instead of trusting the factories.
        Light_Component Sanitized() const noexcept
        {
            const Light_Component defaults;
            const auto finite_non_negative = [](float v, float fallback)
            {
                return (std::isfinite(v) && v >= 0.0f) ? v : fallback;
            };

            Light_Component out = *this;

            out.color.x = finite_non_negative(color.x, defaults.color.x);
            out.color.y = finite_non_negative(color.y, defaults.color.y);
            out.color.z = finite_non_negative(color.z, defaults.color.z);
            out.intensity = finite_non_negative(intensity, defaults.intensity);
            out.range = (std::isfinite(range) && range > 0.0f) ? range : defaults.range;

            const float outer_in = std::isfinite(outer_angle) ? outer_angle : defaults.outer_angle;
            const float inner_in = std::isfinite(inner_angle) ? inner_angle : defaults.inner_angle;

            out.outer_angle = std::clamp(outer_in, MIN_CONE_GAP, MAX_CONE_ANGLE);
            out.inner_angle = std::clamp(inner_in, 0.0f, out.outer_angle - MIN_CONE_GAP);

            return out;
        }

    private:

        // Throws std::invalid_argument naming the first invalid field.
        static Light_Component Checked(const Light_Component& _light, const char* _factory)
        {
            if (const char* field = _light.Invalid_field())
            {
                throw std::invalid_argument(
                    std::string("Light_Component::") + _factory + ": invalid " + field);
            }

            return _light;
        }

    public:

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
            return Checked(light, "Make_directional");
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
            return Checked(light, "Make_point");
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
            return Checked(light, "Make_spot");
        }
    };

} // namespace ECS