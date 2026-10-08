#pragma once

#include <Vector.hpp>
#include <MathConstants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ECS
{

    // Camera_Component: projection parameters for a camera entity.
    //
    // The camera is NOT a standalone class — it is an ECS entity with
    // Transform_Component + Camera_Component. Position and orientation
    // come from the Transform; this component only holds the lens parameters.
    //
    // Only one Camera_Component with is_active = true should exist at a time
    // per render pass. The Extractor searches for the active camera with the
    // lowest render_order and derives the RenderView (view + projection matrices)
    // from its Transform and these parameters.
    //
    // The Camera_Controller operates on the Transform of the camera entity,
    // never on this component directly.
    struct Camera_Component
    {
        // =========================================================
        // Projection type
        // =========================================================

        enum class Projection : uint8_t
        {
            Perspective,    // standard 3D projection — uses fov
            Orthographic,   // flat projection — uses ortho_size, ignores fov
        };

        Projection projection = Projection::Perspective;

        // =========================================================
        // Perspective parameters (Projection::Perspective only)
        // =========================================================

        // Vertical field of view in degrees.
        // 60 degrees is a common default — wide enough to feel natural,
        // narrow enough to avoid excessive perspective distortion.
        float fov = MathLib::Constants::FOV_DEFAULT;

        // =========================================================
        // Orthographic parameters (Projection::Orthographic only)
        // =========================================================

        // Half-height of the orthographic frustum in world units.
        // The full visible height = ortho_size * 2.
        // Width is derived automatically: width = ortho_size * 2 * aspect_ratio.
        // Example: ortho_size = 5.0 → shows 10 world units vertically.
        float ortho_size = 5.0f;

        // =========================================================
        // Clip planes (both projection types)
        // =========================================================

         // Near clip plane distance. Geometry closer than this is clipped.
        //
        // Under Reverse-Z with an infinite far plane this is the ONLY value
        // that affects depth precision (z_ndc = near_plane / distance), so
        // keep it as large as the game tolerates.
        float near_plane = MathLib::Constants::NEAR_PLANE_DEFAULT;

        // Far clip plane distance.
        //
        // IGNORED by Projection::Perspective: that path builds a Reverse-Z
        // matrix with an infinite far plane, so nothing is ever clipped for
        // being too far away and there is no far/near ratio left to spend
        // precision on. Raising or lowering this changes nothing for a
        // perspective camera.
        //
        // Still used by Projection::Orthographic, which has no perspective
        // divide and therefore needs a finite range. Kept as a field for
        // that, and because frustum culling will want a finite bound later.
        float far_plane = MathLib::Constants::FAR_PLANE_DEFAULT;

        // =========================================================
        // Aspect ratio
        // =========================================================

        // Width / height ratio of the viewport.
        // 0.0 = auto: the Extractor computes it from the swapchain extent
        // each frame. Set a non-zero value to override (e.g. for cinematic
        // letterbox or a fixed-aspect render texture in Fase 2).
        float aspect_ratio = 0.0f;

        // =========================================================
        // Rendering priority
        // =========================================================

        // When multiple active cameras exist, the Extractor picks the one
        // with the lowest render_order. Useful for editor (order=0) vs
        // game (order=1) views, or for multi-camera setups in Fase 11.
        int render_order = 0;

        // =========================================================
        // Clear color
        // =========================================================

        // Background color used to clear the framebuffer before rendering.
        // RGBA, linear color space. Alpha is currently unused (opaque window).
        MathLib::Vector4 clear_color = { 0.01f, 0.01f, 0.01f, 1.0f };

        // =========================================================
        // Active flag
        // =========================================================

        // When false this camera is ignored by the Extractor.
        // Allows switching between cameras by toggling is_active.
        bool is_active = true;

        // =========================================================
        // Validation
        // =========================================================

        // True when the parameters describe a usable projection: finite, a
        // positive near plane (the reverse-Z matrix divides by it), a field of
        // view in [FOV_MIN, FOV_MAX] and, for orthographic cameras, a
        // positive size and a far plane beyond the near one.
        bool Is_valid() const noexcept
        {
            return Invalid_field() == nullptr;
        }

        // Name of the first invalid field, or nullptr when the camera is
        // valid. Only the fields the projection type reads are checked.
        const char* Invalid_field() const noexcept
        {
            if (projection != Projection::Perspective && projection != Projection::Orthographic)
                return "projection";

            if (!std::isfinite(near_plane) || near_plane <= 0.0f) return "near_plane";
            if (!std::isfinite(aspect_ratio) || aspect_ratio < 0.0f) return "aspect_ratio";

            if (projection == Projection::Perspective)
            {
                if (!std::isfinite(fov) ||
                    fov < MathLib::Constants::FOV_MIN || fov > MathLib::Constants::FOV_MAX)
                    return "fov";

                return nullptr;
            }

            if (!std::isfinite(ortho_size) || ortho_size <= 0.0f) return "ortho_size";
            if (!std::isfinite(far_plane) || far_plane <= near_plane) return "far_plane (must be > near_plane)";

            return nullptr;
        }

        // Returns a copy whose projection parameters are always safe to
        // build a projection and culling planes from: a non-finite or
        // out of range value falls back to the default (or to the nearest
        // valid value, for the field of view), an aspect ratio that is not
        // usable becomes 0 (automatic) and a far plane that is not beyond
        // the near one is pushed past it. Like Invalid_field(), it only
        // looks at the fields the projection type reads. The fields are
        // public and can change at any time, so the code that fills the
        // RenderPacket uses this instead of trusting whoever set them. A
        // valid camera comes back unchanged.
        Camera_Component Sanitized() const noexcept
        {
            const Camera_Component defaults;
            Camera_Component out = *this;

            if (projection != Projection::Perspective && projection != Projection::Orthographic)
                out.projection = defaults.projection;

            out.near_plane = (std::isfinite(near_plane) && near_plane > 0.0f) ? near_plane : defaults.near_plane;
            out.aspect_ratio = (std::isfinite(aspect_ratio) && aspect_ratio >= 0.0f) ? aspect_ratio : 0.0f;

            if (out.projection == Projection::Perspective)
            {
                out.fov = std::isfinite(fov)
                    ? std::clamp(fov, MathLib::Constants::FOV_MIN, MathLib::Constants::FOV_MAX)
                    : defaults.fov;

                return out;
            }

            out.ortho_size = (std::isfinite(ortho_size) && ortho_size > 0.0f) ? ortho_size : defaults.ortho_size;

            if (!std::isfinite(far_plane) || far_plane <= out.near_plane)
            {
                out.far_plane = std::min(std::max(defaults.far_plane, out.near_plane * 2.0f),
                    MathLib::Constants::FLOAT_MAX);
            }

            return out;
        }

        // =========================================================
        // Convenience constructors
        // =========================================================

        static Camera_Component Make_perspective(float _fov = MathLib::Constants::FOV_DEFAULT, float _near_plane = MathLib::Constants::NEAR_PLANE_DEFAULT,
                                                 float _far_plane = MathLib::Constants::FAR_PLANE_DEFAULT)
        {
            Camera_Component cam;
            cam.projection = Projection::Perspective;
            cam.fov = _fov;
            cam.near_plane = _near_plane;
            cam.far_plane = _far_plane;
            return cam;
        }

        static Camera_Component Make_orthographic(float _ortho_size = 5.0f, float _near_plane = MathLib::Constants::NEAR_PLANE_DEFAULT,
                                                  float _far_plane = MathLib::Constants::FAR_PLANE_DEFAULT)
        {
            Camera_Component cam;
            cam.projection = Projection::Orthographic;
            cam.ortho_size = _ortho_size;
            cam.near_plane = _near_plane;
            cam.far_plane = _far_plane;
            return cam;
        }
    };

} // namespace ECS