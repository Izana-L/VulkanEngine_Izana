#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

namespace Renderer_System 
{
    namespace Vulkan_Buffer_Utils 
    {

        // Creates a VkBuffer and allocates+binds its backing VkDeviceMemory in
        // one call. This is the same "create resource, query memory
        // requirements, allocate, bind" pattern used in Vulkan_Depth_Resources
        // for images, applied here to buffers instead.
        //
        // _size: size in bytes of the buffer
        // _usage: what the buffer will be used for (e.g.
        //   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        //   VK_BUFFER_USAGE_TRANSFER_DST_BIT for staging-buffer destinations)
        // _properties: required memory properties (e.g. DEVICE_LOCAL_BIT for
        //   GPU-only fast memory, or HOST_VISIBLE_BIT | HOST_COHERENT_BIT for
        //   memory the CPU can write to directly without manual flushing)
        // _out_buffer / _out_buffer_memory: the created handles are written here
        void Create_buffer(
            const Vulkan_Device& _device,
            VkDeviceSize _size,
            VkBufferUsageFlags _usage,
            VkMemoryPropertyFlags _properties,
            VkBuffer& _out_buffer,
            VkDeviceMemory& _out_buffer_memory
        );

        // Copies data from _src_buffer to _dst_buffer entirely on the GPU,
        // using a temporary one-off command buffer submitted and waited on
        // immediately. Used for the staging-buffer pattern: upload data to a
        // CPU-visible staging buffer, then copy it into a faster GPU-only
        // (DEVICE_LOCAL) buffer that the CPU can't write to directly.
        //
        // NOTE: this is a simple, synchronous implementation (it blocks until
        // the copy finishes) - fine for occasional uploads like loading a mesh,
        // but not meant to be called every frame in a hot path.
        void Copy_buffer(
            const Vulkan_Device& _device,
            VkCommandPool _command_pool,
            VkBuffer _src_buffer,
            VkBuffer _dst_buffer,
            VkDeviceSize _size
        );

    }
}