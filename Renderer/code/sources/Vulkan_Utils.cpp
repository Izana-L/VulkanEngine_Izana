#include <Vulkan_Utils.hpp>

namespace Renderer_System ::Vulkan_Utils
{
   

        std::string Vk_result_to_string(int32_t _result) 
        {
            switch (_result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:        return "VK_ERROR_OUT_OF_HOST_MEMORY (ran out of system RAM)";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:      return "VK_ERROR_OUT_OF_DEVICE_MEMORY (ran out of GPU memory)";
            case VK_ERROR_INITIALIZATION_FAILED:     return "VK_ERROR_INITIALIZATION_FAILED (generic initialization failure)";
            case VK_ERROR_LAYER_NOT_PRESENT:         return "VK_ERROR_LAYER_NOT_PRESENT (a requested validation layer doesn't exist)";
            case VK_ERROR_EXTENSION_NOT_PRESENT:     return "VK_ERROR_EXTENSION_NOT_PRESENT (a requested extension doesn't exist)";
            case VK_ERROR_INCOMPATIBLE_DRIVER:       return "VK_ERROR_INCOMPATIBLE_DRIVER (GPU driver is not Vulkan-compatible)";
            case VK_ERROR_DEVICE_LOST:               return "VK_ERROR_DEVICE_LOST (the GPU device was lost, e.g. driver crash or TDR)";
            case VK_ERROR_SURFACE_LOST_KHR:          return "VK_ERROR_SURFACE_LOST_KHR (the window surface is no longer valid)";
            case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:  return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR (the native window is already in use)";
            case VK_ERROR_OUT_OF_DATE_KHR:           return "VK_ERROR_OUT_OF_DATE_KHR (the swapchain no longer matches the surface, needs recreation)";
            case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:  return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR (swapchain image layout incompatible with display)";
            case VK_ERROR_TOO_MANY_OBJECTS:          return "VK_ERROR_TOO_MANY_OBJECTS (too many objects of this type already exist)";
            case VK_ERROR_FORMAT_NOT_SUPPORTED:      return "VK_ERROR_FORMAT_NOT_SUPPORTED (the requested format is not supported)";
            default:                                  return "Unknown error code: " + std::to_string(_result);
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
    
}