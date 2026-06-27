#pragma once

#include <Vector.hpp>

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
        float fov = 60.0f;

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
        // Keep as large as possible to maximize depth buffer precision.
        float near_plane = 0.1f;

        // Far clip plane distance. Geometry farther than this is clipped.
        // The ratio far/near determines depth buffer precision — a ratio of
        // 10000 (near=0.1, far=1000) loses significant depth precision.
        // Tighten far or increase near if z-fighting appears.
        float far_plane = 1000.0f;

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
        // Convenience constructors
        // =========================================================

        static Camera_Component Make_perspective(float _fov = 60.0f, float _near_plane = 0.1f,float _far_plane = 1000.0f)
        {
            Camera_Component cam;
            cam.projection = Projection::Perspective;
            cam.fov = _fov;
            cam.near_plane = _near_plane;
            cam.far_plane = _far_plane;
            return cam;
        }

        static Camera_Component Make_orthographic(float _ortho_size = 5.0f, float _near_plane = 0.1f, float _far_plane = 1000.0f)
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