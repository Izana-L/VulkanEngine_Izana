#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
#include <tiny_gltf.h>

// =========================================================
// Stubs requeridos cuando TINYGLTF_NO_STB_IMAGE está definido.
// tinygltf declara estas funciones como externas pero no las
// implementa — hay que proveerlas manualmente.
// En Fase 2 las reemplazaremos por implementaciones reales
// que usen nuestro Image_Loader.
// =========================================================

namespace tinygltf
{
    bool LoadImageData(Image* _image,
        const int           _image_idx,
        std::string* _err,
        std::string* _warn,
        int                 _req_width,
        int                 _req_height,
        const unsigned char* _bytes,
        int                 _size,
        void* _user_data)
    {
        // No-op: image loading handled by our Image_Loader in Fase 2.
        return true;
    }

    bool WriteImageData(const std::string* _basepath,
        const std::string* _filename,
        const Image* _image,
        bool                 _embed_images,
        const FsCallbacks* _fs_cb,
        const URICallbacks* _uri_cb,
        std::string* _out_uri,
        void* _user_data)
    {
        // No-op: we never write images from tinygltf.
        return true;
    }

} // namespace tinygltf