#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>
#include <cstdint>

namespace Renderer_System::Vulkan_Utils 
{

    // Vulkan_utils: shared helper functions used across multiple Vulkan_*
    // classes, to avoid duplicating the same small utilities everywhere.
    
    // Converts a VkResult code into a human-readable string.
    // Vulkan result codes are just integers by default; this turns
    // failures into messages that actually explain what went wrong,
    // instead of a generic "creation failed". Covers every core result
    // code plus the surface/swapchain codes this engine can receive;
    // anything else falls through to the raw numeric value.
    std::string Vk_result_to_string(int32_t _result);

    // Converts a VkFormat into a readable string. Only covers the formats
    // this engine can realistically negotiate for the swapchain; anything
    // else falls through to the raw numeric code.
    std::string Vk_format_to_string(int32_t _format);

    // True if the format applies the automatic linear -> sRGB encode on
    // write. If it does, the fragment shader must NOT apply gamma by hand.
    bool Is_srgb_format(int32_t _format);

    // Result checking, in one place instead of one copy per call site.
    //
    // Check(): throws std::runtime_error("<what>: <readable result>") when
    // _result is an ERROR code (negative). Success codes that carry
    // information (VK_SUBOPTIMAL_KHR, VK_INCOMPLETE, VK_TIMEOUT,
    // VK_NOT_READY) pass through and are returned, so callers that need
    // to act on them can, while every error is reported. Passing a
    // VK_ERROR_DEVICE_LOST through this is what turns a silently ignored
    // GPU reset into a visible failure.
    VkResult Check(VkResult _result, const char* _what);

    // Strict variant: anything other than VK_SUCCESS throws.
    void Check_success(VkResult _result, const char* _what);

}

// Wraps a Vulkan call: VK_CHECK(vkCreateFence(...), "Renderer: create fence").
// The description travels into the exception message together with the
// readable result code.
#define VK_CHECK(expression, what) ::Renderer_System::Vulkan_Utils::Check((expression), (what))
#define VK_CHECK_SUCCESS(expression, what) ::Renderer_System::Vulkan_Utils::Check_success((expression), (what))
