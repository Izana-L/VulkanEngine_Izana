#pragma once

#include <cstdint>

namespace CoreTypes
{

    // Sampler_Preset: the sampling configurations a material can select.
    //
    // The value of each preset IS its index in the bindless sampler array
    // (set 3, binding 1). The Renderer writes the sampler of preset i into
    // slot i at startup, and the value travels to the shader unchanged, so
    // no lookup happens on the CPU per draw. The values are therefore a
    // contract with the GPU side: reordering them silently changes the
    // sampler of every material, and new presets are appended before Count.
    //
    // Linear_Repeat is 0 on purpose: a zero-initialized value always
    // selects a valid, general-purpose sampler.
    //
    // The Vulkan parameters of each preset are defined in the Renderer
    // (Sampler_Cache::Get_preset_desc), which keeps CoreTypes free of
    // Vulkan types, as with Pixel_Format.
    enum class Sampler_Preset : uint32_t
    {
        Linear_Repeat = 0,   // trilinear + anisotropic, wraps: the general case
        Linear_Clamp,        // trilinear + anisotropic, edges clamped: textures that must not bleed across their borders
        Nearest_Repeat,      // point sampling, wraps: pixel art, hard-edged lookups
        Nearest_Clamp,       // point sampling, edges clamped

        Count                // number of presets, not a preset
    };

} // namespace CoreTypes