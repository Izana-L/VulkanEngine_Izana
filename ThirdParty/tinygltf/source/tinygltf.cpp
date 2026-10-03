// The single translation unit that implements tinygltf. Include it through
// Tinygltf_Include.hpp, like every other user: that header defines the
// TINYGLTF_NO_STB_IMAGE* macros, so there are no image callbacks to stub
// here (nothing declares or references tinygltf::LoadImageData / WriteImageData).
#define TINYGLTF_IMPLEMENTATION
#include <Tinygltf_Include.hpp>
