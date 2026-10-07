#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Vulkan_Barrier.hpp>

#include <initializer_list>

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
            Cpu_To_Gpu,

            // GPU writes (transfer), CPU reads. Readback of results such as
            // counters and statistics. VMA prefers HOST_CACHED memory, which
            // is not necessarily HOST_COHERENT: read it through
            // Read_from_buffer, which invalidates the range first.
            Gpu_To_Cpu
        };

        struct Buffer_Allocation
        {
            VkBuffer      buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;

            // Non-null only when _keep_mapped was true. Points straight
            // into the mapped block — no vmaMapMemory needed per write.
            void* mapped_ptr = nullptr;

            // Size in bytes the buffer was created with, so the copies in
            // and out of it can check their range.
            VkDeviceSize size = 0;
        };

        // Creates a VkBuffer and sub-allocates its memory from one of VMA's
        // large blocks, in a single call. Replaces the old
        // create → vkGetBufferMemoryRequirements → vkAllocateMemory → bind
        // sequence: vmaCreateBuffer does all four internally.
        //
        // _keep_mapped: keep a persistent CPU pointer for the lifetime of
        //   the buffer (what Frame_Data's uniform buffer wants). Only valid
        //   with Cpu_To_Gpu and Gpu_To_Cpu.
        //
        // Throws std::invalid_argument, in every build, if _allocator is
        // null, _size is 0, or _keep_mapped is combined with Gpu_Only.
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
        // may simply never reach the GPU on some drivers. The flush is made
        // while the memory is still mapped, as vkFlushMappedMemoryRanges
        // requires.
        //
        // Throws, in every build: std::invalid_argument for an empty
        // (moved-from or destroyed) buffer or null _data, std::out_of_range
        // when _size is larger than the buffer, and Vulkan_Error when the
        // allocation cannot be mapped (it is not host-visible) or flushed.
        // A _size of 0 does nothing.
        void Upload_to_buffer(VmaAllocator             _allocator,
            const Buffer_Allocation& _buffer,
            const void* _data,
            VkDeviceSize             _size);

        // Copies _size bytes from the start of a persistently mapped
        // Gpu_To_Cpu buffer into _destination, after invalidating the
        // mapped range so device writes are visible on memory that is not
        // HOST_COHERENT (a no-op on coherent memory).
        //
        // Precondition: the device writes were made available to the host
        // (a barrier with destination VK_PIPELINE_STAGE_HOST_BIT /
        // VK_ACCESS_HOST_READ_BIT recorded after them) and the submission
        // that wrote them has completed (its fence was waited on).
        //
        // Throws, in every build: std::invalid_argument for an empty buffer,
        // a buffer that was not created with _keep_mapped, or a null
        // _destination, and std::out_of_range when _size is larger than the
        // buffer. A _size of 0 does nothing.
        void Read_from_buffer(VmaAllocator             _allocator,
            const Buffer_Allocation& _buffer,
            void* _destination,
            VkDeviceSize             _size);

        // =========================================================
        // Barriers
        // =========================================================

        // A byte range of a buffer, for commands that touch several
        // buffers at once.
        struct Buffer_Range
        {
            VkBuffer     buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            VkDeviceSize size = VK_WHOLE_SIZE;
        };

        // Records vkCmdFillBuffer(0) on every range of _ranges, then ONE
        // barrier that makes those transfer writes, and every earlier one
        // in the command buffer, visible to _consumer. Resets counters that
        // a compute pass increments with atomics.
        //
        // Every offset and size must be a multiple of 4 (or size
        // VK_WHOLE_SIZE), and every buffer must have been created with
        // VK_BUFFER_USAGE_TRANSFER_DST_BIT. Recorded outside a render pass.
        // The ranges are checked before anything is recorded: a null buffer
        // or a misaligned offset or size throws std::invalid_argument, in
        // every build.
        //
        // Write-after-read against earlier readers of the same ranges is the
        // caller's: nothing here waits for them. Per-frame buffers are
        // ordered by the wait for the last submission of their frame slot.
        void Record_zero_fill_and_barrier(VkCommandBuffer _command_buffer, std::initializer_list<Buffer_Range> _ranges,
                                          const Vulkan_Barrier::Access_Scope& _consumer);

        // Records the barrier from compute shader writes to the work that
        // consumes them: VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT /
        // VK_ACCESS_SHADER_WRITE_BIT on the source side, _consumers on the
        // destination side (fragment reads of a buffer, indirect command
        // reads of draw commands, transfer reads of a counter...).
        void Record_compute_to_consumer_barrier(VkCommandBuffer _command_buffer, const Vulkan_Barrier::Access_Scope& _consumers);
    }
}