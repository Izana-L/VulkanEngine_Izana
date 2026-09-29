#pragma once

#include <Alpha_Mode.hpp>
#include <RenderPacket.hpp>
#include <Sampler_Preset.hpp>
#include <Vector.hpp>

#include <cstdint>

namespace Renderer_System
{

    // Material as the Renderer registers it: GPU-ready values only, with
    // every texture already resolved to its bindless index. The caller
    // (Engine) translates an ECS::Material_Component into this, choosing
    // the default textures for unassigned or missing images, so the
    // Renderer never sees asset handles.
    //
    // Two descriptions that compare equal share one slot of the material
    // table: Register_material deduplicates by value.
    struct Material_Desc
    {
        MathLib::Vector4          base_color = { 1.0f, 1.0f, 1.0f, 1.0f };
        uint32_t                  albedo_texture_index = CoreTypes::Default_Texture::White;
        CoreTypes::Sampler_Preset sampler = CoreTypes::Sampler_Preset::Linear_Repeat;

        // Interpretation of the final alpha in the shaders (see
        // CoreTypes::Alpha_Mode). The pass a material is drawn in follows
        // from it, so the caller that routes draws must use the mode of the
        // registered slot, not a value it can modify afterwards.
        CoreTypes::Alpha_Mode     alpha_mode = CoreTypes::Alpha_Mode::Opaque;

        // Threshold of Alpha_Mode::Mask. Compared as registered for Mask
        // only: two descriptions that differ just in the cutoff of a mode
        // that ignores it are equal (see Normalized_cutoff).
        float                     alpha_cutoff = CoreTypes::DEFAULT_ALPHA_CUTOFF;

        // Cutoff as the GPU entry stores it: the real value for Mask, 0
        // otherwise, so an unused field never splits two equal materials
        // into different slots.
        float Normalized_cutoff() const
        {
            return alpha_mode == CoreTypes::Alpha_Mode::Mask ? alpha_cutoff : 0.0f;
        }

        bool operator==(const Material_Desc& _other) const
        {
            return base_color == _other.base_color
                && albedo_texture_index == _other.albedo_texture_index
                && sampler == _other.sampler
                && alpha_mode == _other.alpha_mode
                && Normalized_cutoff() == _other.Normalized_cutoff();
        }
    };

} // namespace Renderer_System
