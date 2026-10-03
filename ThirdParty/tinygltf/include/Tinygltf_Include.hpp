#pragma once

// The one way to include tinygltf in this engine: include this header, not
// <tiny_gltf.h>.
//
// tinygltf does not decode images here. Textures go through Image_Loader,
// so the library is built without its own stb_image / stb_image_write
// dependency. Those macros change what <tiny_gltf.h> declares (the default
// LoadImageData / WriteImageData callbacks) and what the default member
// initializers of tinygltf::TinyGLTF point to. Every translation unit that
// includes the header must therefore see the same definitions: if one saw
// them and another did not, the same class would have two different
// definitions in one program. Defining them here, once, keeps tinygltf.cpp
// (which implements the library) and every user identical.
//
// Consequence: tinygltf has no default image loader. A caller that loads a
// glTF registers one with TinyGLTF::SetImageLoader (see Mesh_Loader.cpp),
// or loading fails with "No LoadImageData callback specified".

#ifndef TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE
#endif

#ifndef TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#endif

#include <tiny_gltf.h>
