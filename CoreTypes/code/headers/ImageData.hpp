#pragma once

#include <cstdint>
#include <vector>

namespace CoreTypes
{

    // Pixel_Format: CPU-side description of how pixels are laid out.
    // The Renderer maps these to their corresponding VkFormat values
    // internally — VkFormat never appears in CoreTypes.
    enum class Pixel_Format : uint8_t
    {
        RGBA8_UNORM,    // linear color (non-color data: roughness, AO, metallic)
        RGBA8_SRGB,     // sRGB color (albedo, emissive)
        BC7_SRGB,       // block-compressed color (albedo — GPU decompresses)
        BC5_UNORM,      // block-compressed two-channel (normal maps, RG only)
        R8_UNORM,       // single-channel linear (masks, height maps)
        RG8_UNORM,      // two-channel linear (normal maps uncompressed)
        R32_SFLOAT,     // single-channel float (depth, HDR masks)
    };

    // ImageData: CPU-side pixel data produced by ResourceManager
    // (via stb_image or AssetCooker) and consumed by the Renderer
    // to create and upload a VkImage.
    //
    // pixels: raw bytes in row-major order, tightly packed.
    //   Size = width * height * bytes_per_pixel(format) * mip_levels
    //   (mip levels are stored consecutively, largest first).
    // mip_levels: 1 means no mipmaps; the Renderer generates them
    //   at upload time if needed and mip_levels == 1.
    struct ImageData
    {
        std::vector< uint8_t > pixels;
        uint32_t               width = 0;
        uint32_t               height = 0;
        uint32_t               mip_levels = 1;
        Pixel_Format           format = Pixel_Format::RGBA8_SRGB;
    };

} // namespace CoreTypes