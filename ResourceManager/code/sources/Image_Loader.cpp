#include <Image_Loader.hpp>

#include <stb_image.h>

#include <stdexcept>
#include <cstring>

namespace ResourceManager::Image_Loader
{

    CoreTypes::ImageData Load(const std::string& _path,
        CoreTypes::Pixel_Format _format)
    {
        // stb_image loads the file and converts it to RGBA (4 channels)
        // regardless of the source format. The returned pointer must be
        // freed with stbi_image_free() once we've copied the data.
        int width = 0;
        int height = 0;
        int channels = 0;   // original channel count (informational only)

        stbi_uc* pixels = stbi_load(
            _path.c_str(),
            &width,
            &height,
            &channels,
            STBI_rgb_alpha   // force 4-channel output
        );

        if (!pixels) {
            throw std::runtime_error(
                "Image_Loader: failed to load image '" + _path +
                "': " + stbi_failure_reason()
            );
        }

        const size_t data_size =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;

        CoreTypes::ImageData image_data;
        image_data.width = static_cast<uint32_t>(width);
        image_data.height = static_cast<uint32_t>(height);
        image_data.mip_levels = 1;
        image_data.format = _format;
        image_data.pixels.resize(data_size);

        std::memcpy(image_data.pixels.data(), pixels, data_size);

        stbi_image_free(pixels);

        return image_data;
    }

} // namespace ResourceManager::Image_Loader