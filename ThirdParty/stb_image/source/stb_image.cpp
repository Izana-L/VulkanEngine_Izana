// stbi_load receives paths from Platform::Filesystem, which are UTF-8. On
// Windows stb_image would otherwise open them with the ANSI fopen and fail on
// any file name outside the system code page.
#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>