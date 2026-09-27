#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

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
        };

        // Creates a VkBuffer and sub-allocates its memory from one of VMA's
        // large blocks, in a single call. Replaces the old
        // create → vkGetBufferMemoryRequirements → vkAllocateMemory → bind
        // sequence: vmaCreateBuffer does all four internally.
        //
        // _keep_mapped: keep a persistent CPU pointer for the lifetime of
        //   the buffer (what Frame_Data's uniform buffer wants). Only valid
        //   with Cpu_To_Gpu and Gpu_To_Cpu.
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

        // Copies _size bytes from the start of a persistently mapped
        // Gpu_To_Cpu buffer into _destination, after invalidating the
        // mapped range so device writes are visible on memory that is not
        // HOST_COHERENT (a no-op on coherent memory).
        //
        // Precondition: the device writes were made available to the host
        // (a barrier with destination VK_PIPELINE_STAGE_HOST_BIT /
        // VK_ACCESS_HOST_READ_BIT recorded after them) and the submission
        // that wrote them has completed (its fence was waited on).
        void Read_from_buffer(VmaAllocator             _allocator,
            const Buffer_Allocation& _buffer,
            void* _destination,
            VkDeviceSize             _size);

        // =========================================================
        // Barriers
        // =========================================================

        // One side of a memory dependency: the pipeline stages whose work
        // is ordered, and the memory accesses of those stages that are
        // made available (source side) or visible (destination side).
        struct Access_Scope
        {
            VkPipelineStageFlags stages = 0;
            VkAccessFlags        access = 0;
        };

        // A byte range of a buffer, for commands that touch several
        // buffers at once.
        struct Buffer_Range
        {
            VkBuffer     buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            VkDeviceSize size = VK_WHOLE_SIZE;
        };

        // Records one global memory barrier (VkMemoryBarrier) from _source
        // to _destination. A global barrier covers every buffer at once, so
        // several producer/consumer pairs with the same stages are ordered
        // by a single call.
        //
        // An execution-only dependency (write-after-read) passes a _source
        // access of 0: nothing has to be made available, only the order of
        // the stages matters.
        //
        // Throws std::invalid_argument if either stage mask is 0, which
        // vkCmdPipelineBarrier does not allow without synchronization2.
        void Record_memory_barrier(VkCommandBuffer _command_buffer, const Access_Scope& _source, const Access_Scope& _destination);

        // Records vkCmdFillBuffer(0) on every range of _ranges, then ONE
        // barrier that makes those transfer writes, and every earlier one
        // in the command buffer, visible to _consumer. Resets counters that
        // a compute pass increments with atomics.
        //
        // Every offset and size must be a multiple of 4 (or size
        // VK_WHOLE_SIZE), and every buffer must have been created with
        // VK_BUFFER_USAGE_TRANSFER_DST_BIT. Recorded outside a render pass.
        //
        // Write-after-read against earlier readers of the same ranges is the
        // caller's: nothing here waits for them. Per-frame buffers are
        // ordered by the fence of their frame slot.
        void Record_zero_fill_and_barrier(VkCommandBuffer _command_buffer, std::initializer_list<Buffer_Range> _ranges,
                                          const Access_Scope& _consumer);

        // Records the barrier from compute shader writes to the work that
        // consumes them: VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT /
        // VK_ACCESS_SHADER_WRITE_BIT on the source side, _consumers on the
        // destination side (fragment reads of a buffer, indirect command
        // reads of draw commands, transfer reads of a counter...).
        void Record_compute_to_consumer_barrier(VkCommandBuffer _command_buffer, const Access_Scope& _consumers);
    }
}