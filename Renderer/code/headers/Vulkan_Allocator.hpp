#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cstdint>
#include <vk_mem_alloc.h>

#include <Vulkan_Instance.hpp>
#include <Vulkan_Device.hpp>

namespace Renderer_System
{
    // Vulkan_Allocator: owns the VmaAllocator, the single object that
    // sub-allocates every buffer and image in the engine out of a handful
    // of large VkDeviceMemory blocks.
    //
    // Must outlive every buffer/image created through it, and must be
    // destroyed BEFORE Vulkan_Device — hence its position in the
    // Renderer's member list (right after `device`).
    class Vulkan_Allocator
    {
        VmaAllocator allocator;
        uint32_t     heap_count;

    public:

        Vulkan_Allocator(const Vulkan_Instance& _instance,
            const Vulkan_Device& _device);
        ~Vulkan_Allocator();

        Vulkan_Allocator(const Vulkan_Allocator&) = delete;
        Vulkan_Allocator& operator=(const Vulkan_Allocator&) = delete;
        Vulkan_Allocator(Vulkan_Allocator&& _other) noexcept;
        Vulkan_Allocator& operator=(Vulkan_Allocator&& _other) noexcept;

        VmaAllocator Get_handle() const { return allocator; }

        // Prints used / budgeted bytes per memory heap. Cheap enough to
        // call on a keypress, too expensive for every frame.
        void Log_memory_budget() const;

    private:
        void Destroy();
    };
}