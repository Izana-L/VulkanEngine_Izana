#pragma once

#include <cstdint>

namespace CoreTypes
{

    // Alpha_Mode: how a material uses the alpha of its base color (the
    // product of the vertex color, the tint and the albedo texture alpha).
    // The mode is an authoring decision, not something guessed from the
    // value of the alpha, so a texture with cut-outs or a translucent vertex
    // color is never silently drawn solid or silently blended.
    //
    // The values are not a contract with the GPU: the Renderer maps each
    // mode to the GPU_ALPHA_MODE_* macros the shaders read
    // (Material_Table, Gpu_Layouts.hpp), so this enum can stay free of them.
    //
    // Opaque is 0 on purpose: a zero-initialized value is the common case,
    // a solid surface.
    enum class Alpha_Mode : uint8_t
    {
        Opaque = 0,   // the alpha is ignored; drawn solid by the opaque pass
        Mask,         // fragments with alpha below the cutoff are discarded, the rest are solid (foliage, fences); opaque pass
        Blend,        // blended with what is behind: weighted blended transparency, transparent pass

        Count         // number of modes, not a mode
    };

} // namespace CoreTypes
