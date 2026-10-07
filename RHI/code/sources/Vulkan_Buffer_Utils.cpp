#include "Vulkan_Buffer_Utils.hpp"
#include "Vulkan_Utils.hpp"

#include <stdexcept>
#include <cassert>
#include <cstring>
#include <string>

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
            // Enforced in every build: nothing guarantees a validation layer
            // is present, and a release build would otherwise pass these
            // values on to Vulkan and VMA.
            if (_allocator == VK_NULL_HANDLE)
                throw std::invalid_argument("Create_buffer: null allocator");

            if (_size == 0)
                throw std::invalid_argument("Create_buffer: a buffer cannot have a size of 0");

            if (_keep_mapped && _access == Buffer_Access::Gpu_Only)
                throw std::invalid_argument("Create_buffer: GPU-only memory cannot be kept mapped");

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

                alloc_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
                alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

                if (_keep_mapped)
                    alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
            }
            else if (_access == Buffer_Access::Gpu_To_Cpu)
            {
                // RANDOM access lets VMA pick HOST_CACHED memory, the only
                // kind the CPU reads at full speed. Coherence is not
                // required: Read_from_buffer invalidates before reading.
                alloc_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;

                if (_keep_mapped)
                    alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
            }

            VmaAllocationInfo   allocation_info{};
            Buffer_Allocation   out{};

            VK_CHECK(vmaCreateBuffer(
                _allocator,
                &buffer_info,
                &alloc_info,
                &out.buffer,
                &out.allocation,
                &allocation_info), "Failed to create buffer");

            // With MAPPED_BIT, VMA hands back the pointer here — no manual
            // vkMapMemory, and no risk of mapping the same VkDeviceMemory
            // block twice (which Vulkan forbids, and which WOULD happen now
            // that many buffers share one block).
            out.mapped_ptr = allocation_info.pMappedData;
            out.size = _size;

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
                _buffer.size = 0;
            }
        }

        void Upload_to_buffer(VmaAllocator             _allocator,
            const Buffer_Allocation& _buffer,
            const void* _data,
            VkDeviceSize             _size)
        {
            // Enforced in every build: these are the values a release build
            // would otherwise hand to memcpy.
            if (_buffer.allocation == VK_NULL_HANDLE)
                throw std::invalid_argument("Upload_to_buffer: the buffer is empty (moved-from or destroyed)");

            if (_size > _buffer.size)
                throw std::out_of_range("Upload_to_buffer: " + std::to_string(_size) + " bytes do not fit in a buffer of " +
                                        std::to_string(_buffer.size) + " bytes");

            if (_size == 0)
                return;

            if (_data == nullptr)
                throw std::invalid_argument("Upload_to_buffer: null source data");

            void* mapped = _buffer.mapped_ptr;
            const bool mapped_here = (mapped == nullptr);

            if (mapped_here)
            {
                VK_CHECK(vmaMapMemory(_allocator, _buffer.allocation, &mapped),
                    "Upload_to_buffer: allocation is not host-visible");
            }

            std::memcpy(mapped, _data, static_cast<size_t>(_size));

            // Flushed BEFORE the memory is unmapped: vkFlushMappedMemoryRanges
            // requires the memory to be currently host mapped
            // (VUID-VkMappedMemoryRange-memory-00684). A no-op when the
            // memory type happens to be HOST_COHERENT (VMA checks
            // internally), so this is always safe and never wasteful.
            const VkResult flush_result = vmaFlushAllocation(_allocator, _buffer.allocation, 0, _size);

            if (mapped_here)
                vmaUnmapMemory(_allocator, _buffer.allocation);

            VK_CHECK(flush_result, "Upload_to_buffer: flush allocation");
        }

        void Read_from_buffer(VmaAllocator             _allocator,
            const Buffer_Allocation& _buffer,
            void* _destination,
            VkDeviceSize             _size)
        {
            // Enforced in every build, like the checks of Upload_to_buffer.
            if (_buffer.allocation == VK_NULL_HANDLE)
                throw std::invalid_argument("Read_from_buffer: the buffer is empty (moved-from or destroyed)");

            if (_buffer.mapped_ptr == nullptr)
                throw std::invalid_argument("Read_from_buffer: the buffer must be persistently mapped (created with _keep_mapped)");

            if (_size > _buffer.size)
                throw std::out_of_range("Read_from_buffer: " + std::to_string(_size) + " bytes requested from a buffer of " +
                                        std::to_string(_buffer.size) + " bytes");

            if (_size == 0)
                return;

            if (_destination == nullptr)
                throw std::invalid_argument("Read_from_buffer: null destination");

            // No-op when the memory type is HOST_COHERENT, like the flush of
            // Upload_to_buffer.
            VK_CHECK(vmaInvalidateAllocation(_allocator, _buffer.allocation, 0, _size),
                "Read_from_buffer: invalidate allocation");

            std::memcpy(_destination, _buffer.mapped_ptr, static_cast<size_t>(_size));
        }

        void Record_zero_fill_and_barrier(VkCommandBuffer _command_buffer, std::initializer_list<Buffer_Range> _ranges,
                                          const Vulkan_Barrier::Access_Scope& _consumer)
        {
            assert(_command_buffer != VK_NULL_HANDLE);

            // Every range is checked before the first fill is recorded, in
            // every build: vkCmdFillBuffer needs a 4-byte aligned offset and
            // size, and a failure half way would leave part of the fills in
            // the command buffer.
            for (const Buffer_Range& range : _ranges)
            {
                if (range.buffer == VK_NULL_HANDLE)
                    throw std::invalid_argument("Record_zero_fill_and_barrier: null buffer");

                if (range.offset % 4 != 0)
                    throw std::invalid_argument("Record_zero_fill_and_barrier: offset must be a multiple of 4");

                if (range.size != VK_WHOLE_SIZE && range.size % 4 != 0)
                    throw std::invalid_argument("Record_zero_fill_and_barrier: size must be VK_WHOLE_SIZE or a multiple of 4");
            }

            for (const Buffer_Range& range : _ranges)
                vkCmdFillBuffer(_command_buffer, range.buffer, range.offset, range.size, 0u);

            // Fills are transfer writes: without this barrier the consumer
            // may start from the value the previous frame left.
            Vulkan_Barrier::Record_memory_barrier(_command_buffer,
                { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT }, _consumer);
        }

        void Record_compute_to_consumer_barrier(VkCommandBuffer _command_buffer, const Vulkan_Barrier::Access_Scope& _consumers)
        {
            Vulkan_Barrier::Record_memory_barrier(_command_buffer,
                { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT }, _consumers);
        }
    }
}