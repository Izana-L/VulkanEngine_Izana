#include <Image_Loader.hpp>

#include <stb_image.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ResourceManager::Image_Loader
{

    namespace
    {
        // Owns a pixel buffer returned by stb_image and frees it with
        // stbi_image_free() on every exit path, exceptions included.
        struct Stbi_Pixels
        {
            void* data = nullptr;

            Stbi_Pixels() = default;
            Stbi_Pixels(const Stbi_Pixels&) = delete;
            Stbi_Pixels& operator=(const Stbi_Pixels&) = delete;

            ~Stbi_Pixels()
            {
                if (data != nullptr)
                    stbi_image_free(data);
            }
        };

        [[noreturn]] void Throw_load_failure(const std::string& _path)
        {
            const char* reason = stbi_failure_reason();

            throw std::runtime_error("Image_Loader: failed to load image '" + _path + "': " +
                                     (reason != nullptr ? reason : "unknown reason"));
        }

        // Decodes _path as 8-bit RGBA and keeps the first _channels
        // channels of every texel, tightly packed: 4 keeps RGBA, 2 keeps
        // RG, 1 keeps R. stb_image expands grey images to R = G = B, so R
        // is the grey value for them.
        //
        // The channels are selected from RGBA instead of asking stb_image
        // for 1 or 2 components: those return luminance and
        // luminance + alpha, not R and RG.
        std::vector<uint8_t> Decode_8bit(const std::string& _path, uint32_t _channels, int& _out_width, int& _out_height)
        {
            int source_channels = 0;
            Stbi_Pixels pixels;
            pixels.data = stbi_load(_path.c_str(), &_out_width, &_out_height, &source_channels, STBI_rgb_alpha);

            if (pixels.data == nullptr)
                Throw_load_failure(_path);

            const size_t texel_count = static_cast<size_t>(_out_width) * static_cast<size_t>(_out_height);
            const stbi_uc* rgba = static_cast<const stbi_uc*>(pixels.data);

            std::vector<uint8_t> result(texel_count * _channels);

            if (_channels == 4)
            {
                std::memcpy(result.data(), rgba, result.size());
                return result;
            }

            for (size_t texel = 0; texel < texel_count; ++texel)
                for (uint32_t channel = 0; channel < _channels; ++channel)
                    result[texel * _channels + channel] = rgba[texel * 4 + channel];

            return result;
        }

        // Decodes the R channel of _path as a 32-bit float per texel:
        //   HDR (Radiance .hdr) - the stored linear value;
        //   16-bit PNG          - value / 65535;
        //   any other image     - value / 255.
        // The last two are exact normalizations, without the gamma curve
        // stbi_loadf applies to 8-bit images, so a mask or a height map
        // keeps the values it was authored with.
        std::vector<uint8_t> Decode_r32_float(const std::string& _path, int& _out_width, int& _out_height)
        {
            int source_channels = 0;
            Stbi_Pixels pixels;

            enum class Source { Hdr, Bits16, Bits8 };
            Source source = Source::Bits8;

            if (stbi_is_hdr(_path.c_str()))
            {
                pixels.data = stbi_loadf(_path.c_str(), &_out_width, &_out_height, &source_channels, STBI_rgb_alpha);
                source = Source::Hdr;
            }
            else if (stbi_is_16_bit(_path.c_str()))
            {
                pixels.data = stbi_load_16(_path.c_str(), &_out_width, &_out_height, &source_channels, STBI_rgb_alpha);
                source = Source::Bits16;
            }
            else
            {
                pixels.data = stbi_load(_path.c_str(), &_out_width, &_out_height, &source_channels, STBI_rgb_alpha);
            }

            if (pixels.data == nullptr)
                Throw_load_failure(_path);

            const size_t texel_count = static_cast<size_t>(_out_width) * static_cast<size_t>(_out_height);

            std::vector<uint8_t> result(texel_count * sizeof(float));

            for (size_t texel = 0; texel < texel_count; ++texel)
            {
                float value = 0.0f;

                switch (source)
                {
                case Source::Hdr:    value = static_cast<const float*>(pixels.data)[texel * 4]; break;
                case Source::Bits16: value = static_cast<const stbi_us*>(pixels.data)[texel * 4] / 65535.0f; break;
                case Source::Bits8:  value = static_cast<const stbi_uc*>(pixels.data)[texel * 4] / 255.0f; break;
                }

                // memcpy instead of a float* cast: the byte vector is not
                // guaranteed to be aligned for float.
                std::memcpy(result.data() + texel * sizeof(float), &value, sizeof(float));
            }

            return result;
        }
    }

    CoreTypes::ImageData Load(const std::string& _path,
        CoreTypes::Pixel_Format _format)
    {
        // The texel layout of the returned pixels follows _format exactly,
        // so the Renderer's size check (Texture_GPU) and the GPU read the
        // same layout the data was decoded into.
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;

        switch (_format)
        {
        case CoreTypes::Pixel_Format::RGBA8_UNORM:
        case CoreTypes::Pixel_Format::RGBA8_SRGB:
            pixels = Decode_8bit(_path, 4, width, height);
            break;

        case CoreTypes::Pixel_Format::RG8_UNORM:
            pixels = Decode_8bit(_path, 2, width, height);
            break;

        case CoreTypes::Pixel_Format::R8_UNORM:
            pixels = Decode_8bit(_path, 1, width, height);
            break;

        case CoreTypes::Pixel_Format::R32_SFLOAT:
            pixels = Decode_r32_float(_path, width, height);
            break;

        case CoreTypes::Pixel_Format::BC7_SRGB:
        case CoreTypes::Pixel_Format::BC5_UNORM:
        default:
            // stb_image only produces uncompressed texels; block-compressed
            // data has to come already encoded from an offline tool.
            throw std::invalid_argument("Image_Loader: '" + _path + "' cannot be decoded to Pixel_Format " +
                                        std::to_string(static_cast<unsigned>(_format)) +
                                        "; stb_image produces only uncompressed formats");
        }

        CoreTypes::ImageData image_data;
        image_data.width = static_cast<uint32_t>(width);
        image_data.height = static_cast<uint32_t>(height);
        image_data.mip_levels = 1;
        image_data.format = _format;
        image_data.pixels = std::move(pixels);

        return image_data;
    }

} // namespace ResourceManager::Image_Loader
