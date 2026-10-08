#pragma once

#include <Alpha_Mode.hpp>
#include <Asset_Handle.hpp>
#include <Sampler_Preset.hpp>
#include <Vector.hpp>


namespace ECS
{

    // Material_Component: PBR metallic-roughness material data for an entity.
    //
    // Each texture slot holds an Asset_Handle. INVALID_ASSET_HANDLE means
    // "not assigned": the Extractor substitutes one of the default
    // textures the Renderer uploads at startup (CoreTypes::Default_Texture),
    // real 1x1 textures with a neutral value:
    //   albedo            → White (1,1,1,1)
    //   normal            → Flat_Normal (0.5, 0.5, 1.0) in tangent space
    //   metallic_roughness→ White: metallic=metallic_factor, roughness=roughness_factor
    //   ambient_occlusion → White: 1.0 (no occlusion)
    //   emissive          → Black (0,0,0)
    // A slot that IS assigned but whose image has no GPU index (never
    // uploaded, or a stale handle) resolves to Error, magenta.
    // Today only albedo reaches the shader; the other slots follow the
    // same rules when PBR starts reading them.
    //
    // Scalar factors are always applied, even when the corresponding texture
    // is present (they multiply the texture sample, matching glTF spec).
    //
    // Note: Fase 2 will introduce Model_Component { Mesh_Component +
    // Material_Component } to bind geometry and material together.
    // For now, an entity can have both separately.
    struct Material_Component
    {
        // =========================================================
        // Texture slots (INVALID_ASSET_HANDLE = not assigned)
        // =========================================================

        // Base color / albedo map (sRGB).
        CoreTypes::Asset_Handle albedo = CoreTypes::INVALID_ASSET_HANDLE;

        // Tangent-space normal map (linear, RG or RGB).
        CoreTypes::Asset_Handle normal = CoreTypes::INVALID_ASSET_HANDLE;

        // Metallic-roughness map (linear):
        //   R channel = (unused)
        //   G channel = roughness
        //   B channel = metallic
        // Matches glTF 2.0 packing convention.
        CoreTypes::Asset_Handle metallic_roughness = CoreTypes::INVALID_ASSET_HANDLE;

        // Ambient occlusion map (linear, R channel only).
        // Applied as: final_color *= ao_sample * ao_strength (strength = 1.0 for now).
        CoreTypes::Asset_Handle ambient_occlusion = CoreTypes::INVALID_ASSET_HANDLE;

        // Emissive map (sRGB). Multiplied by emissive_factor.
        CoreTypes::Asset_Handle emissive = CoreTypes::INVALID_ASSET_HANDLE;

        // =========================================================
        // Sampling
        // =========================================================

        // Filtering and wrapping applied to every texture slot above. One
        // preset per material for now; per-slot samplers, as glTF allows,
        // are reconsidered together with the glTF loader.
        CoreTypes::Sampler_Preset sampler = CoreTypes::Sampler_Preset::Linear_Repeat;

        // =========================================================
        // Scalar factors
        // =========================================================

        // Base color tint. Multiplies the albedo texture sample (or used directly
        // if no albedo texture). RGBA: the alpha is multiplied with the alpha of
        // the vertex color and of the texture, and what the product is used for
        // is decided by alpha_mode, not by its value.
        MathLib::Vector4 base_color_factor = { 1.0f, 1.0f, 1.0f, 1.0f };

        // How the alpha is used (CoreTypes::Alpha_Mode):
        //   Opaque - ignored: the surface is solid. The default. A tint with
        //            alpha below one does NOT make the material translucent
        //            by itself (Gpu_Assets::Create_material reports it);
        //   Mask   - fragments with alpha below alpha_cutoff are discarded;
        //   Blend  - translucent: drawn by the transparent pass, blended.
        CoreTypes::Alpha_Mode alpha_mode = CoreTypes::Alpha_Mode::Opaque;

        // Alpha_Mode::Mask threshold, in [0, 1]. Ignored by the other modes.
        float alpha_cutoff = 0.5f;

        // Metallic multiplier [0..1]. 0 = dielectric, 1 = full metal.
        float metallic_factor = 1.0f;

        // Roughness multiplier [0..1]. 0 = mirror, 1 = fully rough.
        float roughness_factor = 1.0f;

        // Emissive color scale. Zero = no emission.
        MathLib::Vector3 emissive_factor = { 0.0f, 0.0f, 0.0f };

        // =========================================================
        // GPU binding
        // =========================================================

        // Value of gpu_material_id while the material is not registered.
        static constexpr uint32_t INVALID_GPU_MATERIAL_ID = 0xFFFFFFFFu;

        // Slot of this material in the Renderer's material table, set by
        // Gpu_Assets::Create_material when it registers the material (the
        // fields above, resolved to GPU indices). The Extractor copies it
        // into Draw_Item::material_index; while it is INVALID_GPU_MATERIAL_ID
        // the item draws with CoreTypes::Default_Material.
        //
        // Changing a field above does NOT update the GPU copy: the material
        // has to be registered again with Create_material, which yields a
        // new slot if any value changed (an equal material keeps its slot).
        uint32_t gpu_material_id = INVALID_GPU_MATERIAL_ID;

        // The alpha mode the registered copy has: what alpha_mode was when
        // gpu_material_id was assigned. The Extractor routes each item to
        // the opaque or the transparent pass with this one, never with
        // alpha_mode, so the pass always matches the material the GPU draws
        // with, even if alpha_mode is edited afterwards without registering
        // again. Only meaningful while gpu_material_id is valid; the default
        // material is Opaque.
        CoreTypes::Alpha_Mode gpu_alpha_mode = CoreTypes::Alpha_Mode::Opaque;
    };

} // namespace ECS