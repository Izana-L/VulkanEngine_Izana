// The single translation unit that compiles VMA's implementation.
// VMA_IMPLEMENTATION must be defined in exactly ONE .cpp in the whole
// solution — defining it in a header would duplicate every symbol.
#define VMA_IMPLEMENTATION

// Match the API version Vulkan_Instance requests (VK_API_VERSION_1_3).
// This only tells VMA which Vulkan features it MAY use; the actual
// version is passed per-allocator in VmaAllocatorCreateInfo.
#define VMA_VULKAN_VERSION 1003000

#include <vk_mem_alloc.h>