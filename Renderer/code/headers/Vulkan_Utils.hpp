#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>
#include <cstdint>

namespace Renderer_System::Vulkan_Utils 
{

    // Vulkan_utils: shared helper functions used across multiple Vulkan_*
    // classes, to avoid duplicating the same small utilities everywhere.
    
    // Converts a VkResult error code into a human-readable string.
    // Vulkan error codes are just integers by default - this turns
    // failures into messages that actually explain what went wrong,
    // instead of a generic "creation failed".
    // Covers the most common failure codes relevant during initialization
    // (instance/surface/device/swapchain creation); not an exhaustive
    // list of every VkResult value Vulkan defines.
    std::string Vk_result_to_string(int32_t _result);

    // Converts a VkFormat into a readable string. Only covers the formats
    // this engine can realistically negotiate for the swapchain; anything
    // else falls through to the raw numeric code.
    std::string Vk_format_to_string(int32_t _format);

    // True if the format applies the automatic linear -> sRGB encode on
    // write. If it does, the fragment shader must NOT apply gamma by hand.
    bool Is_srgb_format(int32_t _format);

}