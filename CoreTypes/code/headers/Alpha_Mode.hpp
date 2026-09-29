#pragma once

#include <cstdint>

namespace CoreTypes
{

    // Alpha_Mode: how the alpha of a material is interpreted, with the
    // meaning of glTF 2.0 `alphaMode`. Declared by the material, never
    // inferred from its alpha value: a base color alpha below one does not
    // make a material transparent, and a texture with an alpha channel on
    // a material whose tint alpha is one can still be blended.
    //
    //   Opaque - alpha is ignored; the surface is fully opaque. Drawn in
    //            the opaque pass.
    //   Mask   - alpha test: a fragment whose alpha is below the
    //            material's cutoff is discarded, the rest is fully opaque.
    //            Drawn in the opaque pass.
    //   Blend  - alpha is coverage. Drawn in the transparent pass
    //            (weighted blended order-independent transparency).
    //
    // The values are a contract with the shaders (MATERIAL_ALPHA_MODE_*
    // in scene_data.glsl): reordering them changes the mode every
    // registered material is drawn with.
    enum class Alpha_Mode : uint32_t
    {
        Opaque = 0,
        Mask = 1,
        Blend = 2,

        Count          // number of modes, not a mode
    };

    // Default alpha cutoff of Alpha_Mode::Mask (glTF `alphaCutoff`).
    inline constexpr float DEFAULT_ALPHA_CUTOFF = 0.5f;

    // True for the modes drawn in the transparent pass.
    inline constexpr bool Is_transparent(Alpha_Mode _mode)
    {
        return _mode == Alpha_Mode::Blend;
    }

} // namespace CoreTypes
