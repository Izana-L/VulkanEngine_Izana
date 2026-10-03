#pragma once

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
        uint32_t                  albedo_texture_index = Default_Texture::White;
        CoreTypes::Sampler_Preset sampler = CoreTypes::Sampler_Preset::Linear_Repeat;

        bool operator==(const Material_Desc& _other) const
        {
            return base_color == _other.base_color
                && albedo_texture_index == _other.albedo_texture_index
                && sampler == _other.sampler;
        }
    };

} // namespace Renderer_System
