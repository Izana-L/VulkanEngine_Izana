#include "Vulkan_Buffer_Utils.hpp"
#include "Vulkan_Utils.hpp"

#include <stdexcept>
#include <cassert>
#include <cstring>

namespace Renderer_System
{
    namespace Vulkan_Buffer_Utils
    {
        Buffer_Allocation Create_buffer(
            VmaAllocator       _allocator,
            VkDeviceSize       _size,
            VkBufferUsageFlags _usage,
            Buffer_Access      _access,
            bool               _keep_mapped)
        {
            assert(_size > 0 && "Create_buffer() called with a zero size");
            assert(!(_keep_mapped && _access == Buffer_Access::Gpu_Only) &&
                "Create_buffer(): GPU-only memory cannot be kept mapped");

            // ---------- Buffer description (unchanged) ----------
            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = _size;
            buffer_info.usage = _usage;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            // ---------- Allocation description (this is the new part) ----------
            VmaAllocationCreateInfo alloc_info{};

            // VMA_MEMORY_USAGE_AUTO lets VMA derive the memory type from
            // buffer_info.usage plus the access flags below. The old
            // VMA_MEMORY_USAGE_GPU_ONLY / CPU_ONLY enums are deprecated —
            // they predate ReBAR and unified-memory GPUs and pick badly there.
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

            if (_access == Buffer_Access::Cpu_To_Gpu)
            {
                // SEQUENTIAL_WRITE promises we only memcpy forward and never
                // read back, which lets VMA hand us write-combined memory.
                // Use HOST_ACCESS_RANDOM_BIT instead if you ever need reads.
                alloc_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

                if (_keep_mapped)
                    alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
            }

            VmaAllocationInfo   allocation_info{};
            Buffer_Allocation   out{};

            VkResult result = vmaCreateBuffer(
                _allocator,
                &buffer_info,
                &alloc_info,
                &out.buffer,
                &out.allocation,
                &allocation_info);

            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create buffer: " +
                    Vulkan_Utils::Vk_result_to_string(result));
            }

            // With MAPPED_BIT, VMA hands back the pointer here — no manual
            // vkMapMemory, and no risk of mapping the same VkDeviceMemory
            // block twice (which Vulkan forbids, and which WOULD happen now
            // that many buffers share one block).
            out.mapped_ptr = allocation_info.pMappedData;

            return out;
        }

        void Destroy_buffer(VmaAllocator _allocator, Buffer_Allocation& _buffer)
        {
            if (_buffer.buffer != VK_NULL_HANDLE) {
                // Destroys the buffer AND returns its slice to the block.
                // Tolerates VK_NULL_HANDLE for either argument.
                vmaDestroyBuffer(_allocator, _buffer.buffer, _buffer.allocation);
                _buffer.buffer = VK_NULL_HANDLE;
                _buffer.allocation = VK_NULL_HANDLE;
                _buffer.mapped_ptr = nullptr;
            }
        }

        void Upload_to_buffer(VmaAllocator             _allocator,
            const Buffer_Allocation& _buffer,
            const void* _data,
            VkDeviceSize             _size)
        {
            assert(_buffer.allocation != VK_NULL_HANDLE);

            if (_buffer.mapped_ptr != nullptr)
            {
                std::memcpy(_buffer.mapped_ptr, _data, static_cast<size_t>(_size));
            }
            else
            {
                void* mapped = nullptr;
                vmaMapMemory(_allocator, _buffer.allocation, &mapped);
                std::memcpy(mapped, _data, static_cast<size_t>(_size));
                vmaUnmapMemory(_allocator, _buffer.allocation);
            }

            // No-op when the memory type happens to be HOST_COHERENT (VMA
            // checks internally), so this is always safe and never wasteful.
            vmaFlushAllocation(_allocator, _buffer.allocation, 0, _size);
        }
    }
}