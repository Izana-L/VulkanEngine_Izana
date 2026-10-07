#include <Vulkan_Barrier.hpp>

#include <cassert>
#include <stdexcept>

namespace Renderer_System
{
    namespace Vulkan_Barrier
    {

        // ---------- Record_memory_barrier ----------
        void Record_memory_barrier(VkCommandBuffer _command_buffer, const Access_Scope& _source, const Access_Scope& _destination)
        {
            assert(_command_buffer != VK_NULL_HANDLE);

            if (_source.stages == 0 || _destination.stages == 0)
                throw std::invalid_argument("Record_memory_barrier: both stage masks must be non-zero");

            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = _source.access;
            barrier.dstAccessMask = _destination.access;

            vkCmdPipelineBarrier(_command_buffer, _source.stages, _destination.stages, 0,
                1, &barrier, 0, nullptr, 0, nullptr);
        }

        // ---------- Record_image_barrier ----------
        void Record_image_barrier(
            VkCommandBuffer      _command_buffer,
            VkImage              _image,
            VkImageLayout        _old_layout,
            VkImageLayout        _new_layout,
            const Access_Scope&  _source,
            const Access_Scope&  _destination,
            const Mip_Range&     _mips)
        {
            assert(_command_buffer != VK_NULL_HANDLE &&
                "Record_image_barrier() called with a null command buffer");
            assert(_image != VK_NULL_HANDLE &&
                "Record_image_barrier() called with a null image");

            // Enforced in every build: an empty stage mask is invalid usage
            // without synchronization2, which the device does not enable,
            // and nothing guarantees a validation layer is present to
            // report it. The same goes for an empty range of mip levels.
            if (_source.stages == 0 || _destination.stages == 0)
                throw std::invalid_argument("Record_image_barrier: the source and destination stage masks must not be empty");

            if (_mips.level_count == 0)
                throw std::invalid_argument("Record_image_barrier: the range of mip levels must not be empty");

            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = _old_layout;
            barrier.newLayout = _new_layout;
            barrier.srcAccessMask = _source.access;
            barrier.dstAccessMask = _destination.access;

            // No queue family ownership transfer: the barrier stays within
            // the queue family that records it.
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            barrier.image = _image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = _mips.base_level;
            barrier.subresourceRange.levelCount = _mips.level_count;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;

            vkCmdPipelineBarrier(_command_buffer, _source.stages, _destination.stages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

    }
}
