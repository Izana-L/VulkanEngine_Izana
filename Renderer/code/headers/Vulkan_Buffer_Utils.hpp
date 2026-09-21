#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

namespace Renderer_System
{
    namespace Vulkan_Buffer_Utils
    {
        // How the CPU will touch this buffer. Replaces the raw
        // VkMemoryPropertyFlags the old signature took: VMA picks the
        // actual memory type from the intent, which is both harder to get
        // wrong and adapts to the GPU (unified memory, ReBAR, etc.).
        enum class Buffer_Access
        {
            // GPU-only. Vertex/index buffers after upload, render targets.
            Gpu_Only,

            // CPU writes sequentially, GPU reads. Staging buffers and
            // per-frame uniform buffers. Never read back from this memory:
            // it is usually write-combined and CPU reads are ~100x slower.
            Cpu_To_Gpu
        };

        struct Buffer_Allocation
        {
            VkBuffer      buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;

            // Non-null only when _keep_mapped was true. Points straight
            // into the mapped block — no vmaMapMemory needed per write.
            void* mapped_ptr = nullptr;
        };

        // Creates a VkBuffer and sub-allocates its memory from one of VMA's
        // large blocks, in a single call. Replaces the old
        // create → vkGetBufferMemoryRequirements → vkAllocateMemory → bind
        // sequence: vmaCreateBuffer does all four internally.
        //
        // _keep_mapped: keep a persistent CPU pointer for the lifetime of
        //   the buffer (what Frame_Data's uniform buffer wants). Only valid
        //   with Cpu_To_Gpu.
        Buffer_Allocation Create_buffer(
            VmaAllocator       _allocator,
            VkDeviceSize       _size,
            VkBufferUsageFlags _usage,
            Buffer_Access      _access,
            bool               _keep_mapped = false);

        // Frees both the VkBuffer and its allocation. Replaces the
        // vkDestroyBuffer + vkFreeMemory pair. Safe with null handles.
        void Destroy_buffer(VmaAllocator _allocator, Buffer_Allocation& _buffer);

        // Copies host data into a mapped (or mappable) allocation and
        // flushes it. Use this instead of a bare memcpy: VMA's AUTO usage
        // does not guarantee HOST_COHERENT memory, and an unflushed write
        // may simply never reach the GPU on some drivers.
        void Upload_to_buffer(VmaAllocator             _allocator,
            const Buffer_Allocation& _buffer,
            const void* _data,
            VkDeviceSize             _size);
    }
}