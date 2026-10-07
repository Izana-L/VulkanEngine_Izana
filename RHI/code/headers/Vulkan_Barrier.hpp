#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Renderer_System
{
    // Vulkan_Barrier: the one place that records pipeline barriers, shared
    // by the buffer and image utilities, the texture upload, the storage
    // images and the GPU timer. Every barrier states its synchronization
    // explicitly, as two Access_Scopes; none is derived from layouts.
    namespace Vulkan_Barrier
    {
        // One side of a memory dependency: the pipeline stages whose work
        // is ordered, and the memory accesses of those stages that are
        // made available (source side) or visible (destination side).
        struct Access_Scope
        {
            VkPipelineStageFlags stages = 0;
            VkAccessFlags        access = 0;
        };

        // The mip levels of an image that a barrier covers.
        struct Mip_Range
        {
            uint32_t base_level = 0;
            uint32_t level_count = 1;
        };

        // Records one global memory barrier (VkMemoryBarrier) from _source
        // to _destination. A global barrier covers every buffer and image
        // at once, so several producer/consumer pairs with the same stages
        // are ordered by a single call.
        //
        // An execution-only dependency (write-after-read) passes a _source
        // access of 0: nothing has to be made available, only the order of
        // the stages matters.
        //
        // Throws std::invalid_argument if either stage mask is 0, which
        // vkCmdPipelineBarrier does not allow without synchronization2.
        void Record_memory_barrier(VkCommandBuffer _command_buffer, const Access_Scope& _source, const Access_Scope& _destination);

        // Records an image memory barrier on the color aspect of the mip
        // levels _mips (array layer 0), with the synchronization stated by
        // the caller instead of derived from the layouts:
        //   _source      - the earlier work the barrier waits for, and the
        //                  writes it makes available;
        //   _destination - the later work that waits for the barrier, and
        //                  the accesses the data is made visible to.
        // A layout pair alone does not say which work happens on each side
        // (GENERAL serves storage writes by any shader stage, compute to
        // compute chains through imageLoad, transfer writes...), so every
        // transition whose layouts do not identify that work is recorded
        // here, never through Vulkan_Image_Utils::Transition_image_layout.
        //
        // The caller is responsible for what only it can know: every stage
        // in both scopes must be supported by the queue family of the pool
        // _command_buffer was allocated from (FRAGMENT_SHADER, for example,
        // is not available on a compute-only queue), and every access must
        // be supported by a stage of its scope.
        //
        // _command_buffer: must already be in the recording state.
        //
        // Throws std::invalid_argument if either stage mask is 0 (which
        // vkCmdPipelineBarrier does not allow without synchronization2) or
        // _mips is empty.
        void Record_image_barrier(
            VkCommandBuffer      _command_buffer,
            VkImage              _image,
            VkImageLayout        _old_layout,
            VkImageLayout        _new_layout,
            const Access_Scope&  _source,
            const Access_Scope&  _destination,
            const Mip_Range&     _mips);
    }
}
