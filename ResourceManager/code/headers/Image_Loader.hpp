#pragma once

#include <ImageData.hpp>

#include <string>

namespace ResourceManager::Image_Loader
{

    // Loads an image from disk using stb_image and returns it as
    // a CoreTypes::ImageData ready to be uploaded to the GPU.
    //
    // Supported formats: PNG, JPG, TGA, BMP, HDR (8-bit channels).
    // Output is always 4 channels (RGBA) regardless of source format —
    // stb_image pads missing channels (e.g. RGB → RGBA with alpha = 255).
    //
    // _format: how the Renderer should interpret the pixel data.
    //   Use RGBA8_SRGB  for color textures  (albedo, emissive).
    //   Use RGBA8_UNORM for non-color data   (roughness, AO, metallic).
    //   Use RG8_UNORM   for normal maps      (uncompressed).
    //
    // Throws std::runtime_error if the file cannot be opened or parsed.
    CoreTypes::ImageData Load(const std::string& _path,
        CoreTypes::Pixel_Format  _format =
        CoreTypes::Pixel_Format::RGBA8_SRGB);

} // namespace ResourceManager::Image_Loader