#include "Vulkan_Buffer_Utils.hpp"
#include "Vulkan_Utils.hpp"

#include <stdexcept>
#include <cassert>

namespace Renderer_System 
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
            alloc_info.memoryTypeIndex = _device.Find_memory_type(memory_requirements.memoryTypeBits,_properties);
                
                

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

       

    }
}