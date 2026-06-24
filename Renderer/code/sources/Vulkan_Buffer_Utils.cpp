#include "Vulkan_Buffer_Utils.hpp"
#include "Vulkan_Utils.hpp"

#include <stdexcept>
#include <cassert>

namespace Renderer 
{
    namespace Vulkan_Buffer_Utils 
    {

        // ---------- Create_buffer ----------
        void Create_buffer(
            const Vulkan_Device& _device,
            VkDeviceSize _size,
            VkBufferUsageFlags _usage,
            VkMemoryPropertyFlags _properties,
            VkBuffer& _out_buffer,
            VkDeviceMemory& _out_buffer_memory) {

            assert(_size > 0 && "Create_buffer() called with a zero size");

            VkDevice device_handle = _device.Get_logical_device_handle();

            // ---------- Buffer creation ----------
            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = _size;
            buffer_info.usage = _usage;

            // EXCLUSIVE: this buffer will only ever be used by one queue
            // family at a time (the graphics queue) - no need for the more
            // expensive CONCURRENT sharing mode here.
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VkResult result = vkCreateBuffer(device_handle, &buffer_info, nullptr, &_out_buffer);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create buffer: " + Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            // ---------- Memory allocation ----------
            VkMemoryRequirements memory_requirements{};
            vkGetBufferMemoryRequirements(device_handle, _out_buffer, &memory_requirements);

            VkMemoryAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc_info.allocationSize = memory_requirements.size;
            alloc_info.memoryTypeIndex = _device.Find_memory_type(
                memory_requirements.memoryTypeBits,
                _properties
            );

            result = vkAllocateMemory(device_handle, &alloc_info, nullptr, &_out_buffer_memory);
            if (result != VK_SUCCESS) {
                // Clean up the buffer we already created, since the overall
                // operation failed - avoid leaking it.
                vkDestroyBuffer(device_handle, _out_buffer, nullptr);
                _out_buffer = VK_NULL_HANDLE;

                throw std::runtime_error(
                    "Failed to allocate buffer memory: " + Vulkan_Utils::Vk_result_to_string(result)
                );
            }

            // Bind the allocated memory to the buffer - offset 0 since this
            // allocation is dedicated entirely to this one buffer.
            vkBindBufferMemory(device_handle, _out_buffer, _out_buffer_memory, 0);
        }

        // ---------- Copy_buffer ----------
        void Copy_buffer(
            const Vulkan_Device& _device,
            VkCommandPool _command_pool,
            VkBuffer _src_buffer,
            VkBuffer _dst_buffer,
            VkDeviceSize _size) {

            assert(_src_buffer != VK_NULL_HANDLE && "Copy_buffer() called with a null source buffer");
            assert(_dst_buffer != VK_NULL_HANDLE && "Copy_buffer() called with a null destination buffer");
            assert(_size > 0 && "Copy_buffer() called with a zero size");

            VkDevice device_handle = _device.Get_logical_device_handle();

            // ---------- Allocate a temporary, one-off command buffer ----------
            VkCommandBufferAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc_info.commandPool = _command_pool;
            alloc_info.commandBufferCount = 1;

            VkCommandBuffer command_buffer;
            vkAllocateCommandBuffers(device_handle, &alloc_info, &command_buffer);

            // ---------- Record the copy command ----------
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

            // ONE_TIME_SUBMIT_BIT tells the driver this buffer will only be
            // submitted once, allowing it to optimize accordingly.
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

            vkBeginCommandBuffer(command_buffer, &begin_info);

            VkBufferCopy copy_region{};
            copy_region.srcOffset = 0;
            copy_region.dstOffset = 0;
            copy_region.size = _size;

            vkCmdCopyBuffer(command_buffer, _src_buffer, _dst_buffer, 1, &copy_region);

            vkEndCommandBuffer(command_buffer);

            // ---------- Submit and wait synchronously ----------
            VkSubmitInfo submit_info{};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &command_buffer;

            VkQueue graphics_queue = _device.Get_graphics_queue();
            vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE);

            // Block until the copy actually completes. This is the simple,
            // synchronous approach - acceptable for occasional uploads (like
            // loading a mesh once), but would be a bottleneck if called every
            // frame, since it stalls the entire graphics queue.
            vkQueueWaitIdle(graphics_queue);

            // The command buffer is no longer needed once the copy is done.
            vkFreeCommandBuffers(device_handle, _command_pool, 1, &command_buffer);
        }

    }
}