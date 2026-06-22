#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>
#include <cstdint>

namespace Renderer::Vulkan_utils 
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

    

}