#pragma once

#include <ImageData.hpp>

#include <string>

namespace ResourceManager::Image_Loader
{

    // Loads an image from disk using stb_image and returns it as
    // a CoreTypes::ImageData ready to be uploaded to the GPU.
    //
    // Supported files: PNG, JPG, TGA, BMP, PSD, GIF, HDR, PIC, PNM.
    //
    // _format: how the Renderer should interpret the pixel data. The
    // pixels are decoded into exactly the texel layout of _format, so the
    // buffer always matches what the GPU reads:
    //   RGBA8_SRGB  - 4 bytes per texel; color textures (albedo, emissive).
    //   RGBA8_UNORM - 4 bytes per texel; non-color data (roughness, AO,
    //                 metallic).
    //   RG8_UNORM   - 2 bytes per texel, the R and G channels of the file;
    //                 uncompressed normal maps.
    //   R8_UNORM    - 1 byte per texel, the R channel of the file (the grey
    //                 value for greyscale files); masks, height maps.
    //   R32_SFLOAT  - one float per texel, the R channel of the file: the
    //                 stored value for HDR files, normalized to [0, 1] for
    //                 8-bit (/ 255) and 16-bit (/ 65535) files.
    // Channels missing from the file are filled by stb_image (grey is
    // copied to R, G and B; alpha is 255).
    //
    // Throws std::runtime_error if the file cannot be opened or parsed,
    // and std::invalid_argument for the block-compressed formats
    // (BC7_SRGB, BC5_UNORM), which stb_image cannot produce.
    CoreTypes::ImageData Load(const std::string& _path,
        CoreTypes::Pixel_Format  _format =
        CoreTypes::Pixel_Format::RGBA8_SRGB);

} // namespace ResourceManager::Image_Loader