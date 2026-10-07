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
    // pixels: raw bytes in row-major order, tightly packed. The mip levels
    //   are stored consecutively, largest first, and level L has
    //   max(width >> L, 1) x max(height >> L, 1) texels, so
    //   Size = the sum, over the mip_levels levels, of
    //   max(width >> L, 1) * max(height >> L, 1) * bytes_per_pixel(format).
    // mip_levels: how many levels of the chain `pixels` holds, at least 1.
    //   1 is the base level alone: the Renderer generates the rest of the
    //   chain on the GPU at upload time. A larger value (a texture cooked
    //   offline with its own mips) is uploaded as it is, and only the levels
    //   it lacks, if any, are generated from the last one it holds.
    struct ImageData
    {
        std::vector< uint8_t > pixels;
        uint32_t               width = 0;
        uint32_t               height = 0;
        uint32_t               mip_levels = 1;
        Pixel_Format           format = Pixel_Format::RGBA8_SRGB;
    };

} // namespace CoreTypes