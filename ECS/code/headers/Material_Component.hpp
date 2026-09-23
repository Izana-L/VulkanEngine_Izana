#pragma once

#include <Asset_Handle.hpp>
#include <Sampler_Preset.hpp>
#include <Vector.hpp>


namespace ECS
{

    // Material_Component: PBR metallic-roughness material data for an entity.
    //
    // Each texture slot holds an Asset_Handle. INVALID_ASSET_HANDLE means
    // "not assigned" — the Extractor/shader falls back to a default:
    //   albedo            → white (1,1,1,1)
    //   normal            → flat normal (0.5, 0.5, 1.0) in tangent space
    //   metallic_roughness→ metallic=metallic_factor, roughness=roughness_factor
    //   ambient_occlusion → 1.0 (no occlusion)
    //   emissive          → black (0,0,0)
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
        // if no albedo texture). RGBA — alpha is reserved for future transparency.
        MathLib::Vector4 base_color_factor = { 1.0f, 1.0f, 1.0f, 1.0f };

        // Metallic multiplier [0..1]. 0 = dielectric, 1 = full metal.
        float metallic_factor = 1.0f;

        // Roughness multiplier [0..1]. 0 = mirror, 1 = fully rough.
        float roughness_factor = 1.0f;

        // Emissive color scale. Zero = no emission.
        MathLib::Vector3 emissive_factor = { 0.0f, 0.0f, 0.0f };
    };

} // namespace ECS