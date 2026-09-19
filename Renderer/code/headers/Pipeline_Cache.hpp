#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Renderer_System
{

    // Pipeline_Cache: owns the VkPipelineCache and its on-disk blob.
    //
    // What it buys: nothing on a cold first run — the cache starts empty.
    // On every run after that the driver reuses the compiled result instead
    // of recompiling from SPIR-V. It does NOT decide *when* compilation
    // happens; that's what warm-up (Paso 4) is for. The two are not
    // alternatives: warm-up is what fills this, and this is what makes
    // warm-up cheap from the second run on.
    //
    // The blob is driver- and device-specific. A GPU swap or a driver update
    // invalidates it — Is_blob_usable() checks that before handing anything
    // to Vulkan, so a stale file degrades to "start empty" instead of
    // undefined behaviour.
    class Pipeline_Cache
    {
        VkDevice         device_handle;
        VkPhysicalDevice physical_device;
        VkPipelineCache  cache;
        std::string      file_path;

    public:

        explicit Pipeline_Cache(const Vulkan_Device& _device);
        ~Pipeline_Cache();

        Pipeline_Cache(const Pipeline_Cache&) = delete;
        Pipeline_Cache& operator=(const Pipeline_Cache&) = delete;
        Pipeline_Cache(Pipeline_Cache&&) = delete;
        Pipeline_Cache& operator=(Pipeline_Cache&&) = delete;

        // Passed to vkCreateGraphicsPipelines. Never VK_NULL_HANDLE after
        // construction — an empty cache is still a perfectly valid cache.
        VkPipelineCache Get_handle() const { return cache; }

        // Serializes the current contents to disk. Called by the destructor;
        // public so a long session can checkpoint without shutting down.
        bool Save() const;

    private:

        // Rejects a blob this driver/GPU can't use: too short, wrong header
        // version, different vendor, device, or cache UUID.
        bool Is_blob_usable(const std::vector<uint8_t>& _blob) const;
    };

} // namespace Renderer_System