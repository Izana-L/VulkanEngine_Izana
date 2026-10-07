#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>
#include <cstdint>
#include <type_traits>
#include <vector>

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
    // this engine can realistically negotiate (swapchain, depth buffer and
    // the texture formats it uploads); anything else falls through to the
    // raw numeric code.
    std::string Vk_format_to_string(int32_t _format);

    // True if the format applies the automatic linear -> sRGB encode on
    // write. If it does, the fragment shader must NOT apply gamma by hand.
    bool Is_srgb_format(int32_t _format);

    // The exception Check and Check_success throw: a std::runtime_error
    // that also carries the VkResult that caused it, so a handler can tell
    // a lost device (VK_ERROR_DEVICE_LOST, after which no Vulkan call can be
    // relied on) from an error the program can recover from, without
    // parsing the message.
    class Vulkan_Error : public std::runtime_error
    {
    public:

        Vulkan_Error(VkResult _result, const std::string& _message)
            : std::runtime_error(_message), result(_result)
        {
        }

        VkResult Get_result() const noexcept { return result; }

        bool Is_device_lost() const noexcept { return result == VK_ERROR_DEVICE_LOST; }

    private:

        VkResult result;
    };

    // Result checking, in one place instead of one copy per call site.
    //
    // Check(): throws Vulkan_Error("<what>: <readable result>") when
    // _result is an ERROR code (negative). Success codes that carry
    // information (VK_SUBOPTIMAL_KHR, VK_INCOMPLETE, VK_TIMEOUT,
    // VK_NOT_READY) pass through and are returned, so callers that need
    // to act on them can, while every error is reported. Passing a
    // VK_ERROR_DEVICE_LOST through this is what turns a silently ignored
    // GPU reset into a visible failure.
    VkResult Check(VkResult _result, const char* _what);

    // Strict variant: anything other than VK_SUCCESS throws Vulkan_Error.
    void Check_success(VkResult _result, const char* _what);

    // Reads a Vulkan array with the two-call pattern (first the count, then
    // the data) and returns exactly the elements the driver wrote.
    //
    // _call receives (uint32_t* count, T* data) and forwards them to the
    // Vulkan function, for example:
    //
    //   Enumerate<VkPhysicalDevice>([&](uint32_t* _count, VkPhysicalDevice* _data)
    //       { return vkEnumeratePhysicalDevices(instance, _count, _data); }, "enumerate physical devices");
    //
    // The two calls are not atomic: the list can change between them (a GPU
    // plugged in, a window moved to another monitor with other surface
    // formats). A function that returns VkResult reports it with
    // VK_INCOMPLETE, and the query is repeated from the count until a call
    // is complete, instead of keeping a truncated list. When the list
    // shrinks, the vector is cut to the number of elements written, so it
    // never ends with value-initialized entries (null handles, for example)
    // that no driver wrote. A failed call throws Vulkan_Error. Functions
    // that return void (vkGetPhysicalDeviceQueueFamilyProperties) have no
    // way to report an incomplete list and are queried once.
    //
    // T must be default-constructible; the Vulkan structures that carry an
    // sType (the *2 queries) are not supported.
    template <typename T, typename Call>
    std::vector<T> Enumerate(Call _call, const char* _what)
    {
        std::vector<T> items;

        if constexpr (std::is_void_v<std::invoke_result_t<Call&, uint32_t*, T*>>)
        {
            uint32_t count = 0;
            _call(&count, static_cast<T*>(nullptr));

            items.resize(count);
            _call(&count, items.data());

            items.resize(count);
            return items;
        }
        else
        {
            // A driver that keeps answering VK_INCOMPLETE would otherwise
            // loop forever; no real list changes this many times in a row.
            constexpr int MAX_ATTEMPTS = 8;

            for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
            {
                uint32_t count = 0;
                Check(_call(&count, static_cast<T*>(nullptr)), _what);

                items.resize(count);
                const VkResult result = Check(_call(&count, items.data()), _what);

                items.resize(count);

                if (result != VK_INCOMPLETE)
                    return items;
            }

            throw Vulkan_Error(VK_INCOMPLETE, std::string(_what) + ": the list kept changing between the count and the data queries");
        }
    }

}

// Wraps a Vulkan call: VK_CHECK(vkCreateFence(...), "Renderer: create fence").
// The description travels into the exception message together with the
// readable result code.
#define VK_CHECK(expression, what) ::Renderer_System::Vulkan_Utils::Check((expression), (what))
#define VK_CHECK_SUCCESS(expression, what) ::Renderer_System::Vulkan_Utils::Check_success((expression), (what))
