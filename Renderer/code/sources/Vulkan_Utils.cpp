#include <Vulkan_Utils.hpp>

#include <stdexcept>

namespace Renderer_System ::Vulkan_Utils
{

    std::string Vk_result_to_string(int32_t _result) 
    {
        switch (_result) {
        // Success codes
        case VK_SUCCESS:                          return "VK_SUCCESS";
        case VK_NOT_READY:                        return "VK_NOT_READY (a fence or query has not completed yet)";
        case VK_TIMEOUT:                          return "VK_TIMEOUT (a wait operation has not completed in the specified time)";
        case VK_EVENT_SET:                        return "VK_EVENT_SET";
        case VK_EVENT_RESET:                      return "VK_EVENT_RESET";
        case VK_INCOMPLETE:                       return "VK_INCOMPLETE (a return array was too small for the result)";
        case VK_SUBOPTIMAL_KHR:                   return "VK_SUBOPTIMAL_KHR (the swapchain no longer matches the surface exactly, but can still present)";
        // Core error codes
        case VK_ERROR_OUT_OF_HOST_MEMORY:         return "VK_ERROR_OUT_OF_HOST_MEMORY (ran out of system RAM)";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:       return "VK_ERROR_OUT_OF_DEVICE_MEMORY (ran out of GPU memory)";
        case VK_ERROR_INITIALIZATION_FAILED:      return "VK_ERROR_INITIALIZATION_FAILED (generic initialization failure)";
        case VK_ERROR_DEVICE_LOST:                return "VK_ERROR_DEVICE_LOST (the GPU device was lost, e.g. driver crash or TDR)";
        case VK_ERROR_MEMORY_MAP_FAILED:          return "VK_ERROR_MEMORY_MAP_FAILED (mapping of a memory object has failed)";
        case VK_ERROR_LAYER_NOT_PRESENT:          return "VK_ERROR_LAYER_NOT_PRESENT (a requested validation layer doesn't exist)";
        case VK_ERROR_EXTENSION_NOT_PRESENT:      return "VK_ERROR_EXTENSION_NOT_PRESENT (a requested extension doesn't exist)";
        case VK_ERROR_FEATURE_NOT_PRESENT:        return "VK_ERROR_FEATURE_NOT_PRESENT (a requested feature is not supported)";
        case VK_ERROR_INCOMPATIBLE_DRIVER:        return "VK_ERROR_INCOMPATIBLE_DRIVER (GPU driver is not Vulkan-compatible)";
        case VK_ERROR_TOO_MANY_OBJECTS:           return "VK_ERROR_TOO_MANY_OBJECTS (too many objects of this type already exist)";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:       return "VK_ERROR_FORMAT_NOT_SUPPORTED (the requested format is not supported)";
        case VK_ERROR_FRAGMENTED_POOL:            return "VK_ERROR_FRAGMENTED_POOL (a pool allocation failed due to fragmentation)";
        case VK_ERROR_UNKNOWN:                    return "VK_ERROR_UNKNOWN (an unknown error occurred; usually invalid API usage)";
        case VK_ERROR_OUT_OF_POOL_MEMORY:         return "VK_ERROR_OUT_OF_POOL_MEMORY (a descriptor or command pool is exhausted)";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE:    return "VK_ERROR_INVALID_EXTERNAL_HANDLE (an external handle is invalid)";
        case VK_ERROR_FRAGMENTATION:              return "VK_ERROR_FRAGMENTATION (a descriptor pool creation failed due to fragmentation)";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS (invalid buffer or memory capture address)";
        // Surface / swapchain error codes
        case VK_ERROR_SURFACE_LOST_KHR:           return "VK_ERROR_SURFACE_LOST_KHR (the window surface is no longer valid)";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:   return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR (the native window is already in use)";
        case VK_ERROR_OUT_OF_DATE_KHR:            return "VK_ERROR_OUT_OF_DATE_KHR (the swapchain no longer matches the surface, needs recreation)";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:   return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR (swapchain image layout incompatible with display)";
        case VK_ERROR_VALIDATION_FAILED_EXT:      return "VK_ERROR_VALIDATION_FAILED_EXT (a validation layer rejected the call)";
        case VK_ERROR_INVALID_SHADER_NV:          return "VK_ERROR_INVALID_SHADER_NV (a shader failed to compile or link)";
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT (exclusive full-screen access was lost)";
        default:                                  return "VkResult(" + std::to_string(_result) + ")";
        }
    }

    std::string Vk_format_to_string(int32_t _format) 
    {
        switch (_format) {
        case VK_FORMAT_B8G8R8A8_SRGB:            return "VK_FORMAT_B8G8R8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_SRGB:            return "VK_FORMAT_R8G8B8A8_SRGB";
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:     return "VK_FORMAT_A8B8G8R8_SRGB_PACK32";
        case VK_FORMAT_B8G8R8A8_UNORM:           return "VK_FORMAT_B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_UNORM:           return "VK_FORMAT_R8G8B8A8_UNORM";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_R16G16B16A16_SFLOAT:      return "VK_FORMAT_R16G16B16A16_SFLOAT";
        default:                                 return "VkFormat(" + std::to_string(_format) + ")";
        }
    }

    bool Is_srgb_format(int32_t _format) 
    {
        switch (_format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return true;
        default:                             return false;
        }
    }

    VkResult Check(VkResult _result, const char* _what)
    {
        if (_result < 0)
        {
            throw std::runtime_error(std::string(_what) + ": " + Vk_result_to_string(_result));
        }

        return _result;
    }

    void Check_success(VkResult _result, const char* _what)
    {
        if (_result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string(_what) + ": " + Vk_result_to_string(_result));
        }
    }
    
}
