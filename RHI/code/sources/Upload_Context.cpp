#include <Upload_Context.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>

namespace Renderer_System
{

    Upload_Context::Upload_Context(const Vulkan_Device& _device, VmaAllocator _allocator)
        : device(_device),
        allocator(_allocator),
        command_pool(_device, 0, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT),
        fence(Create_fence(_device.Get_logical_device_handle(), false, "Upload_Context: failed to create the transfer fence"))
    {
    }

    Upload_Context::~Upload_Context()
    {
        // The staging buffers go first, then the members (the fence and the
        // command pool) in reverse order of declaration.
        Destroy_staging_buffers();
    }

    void Upload_Context::Destroy_staging_buffers() noexcept
    {
        for (Vulkan_Buffer_Utils::Buffer_Allocation& buffer : staging_buffers)
            Vulkan_Buffer_Utils::Destroy_buffer(allocator, buffer);

        staging_buffers.clear();
    }

    VkCommandBuffer Upload_Context::Begin()
    {
        VkCommandBuffer command_buffer = command_pool.Allocate_primary();

        try
        {
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

            VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info), "Upload_Context: begin transfer command buffer");
        }
        catch (...)
        {
            command_pool.Free(command_buffer);
            throw;
        }

        return command_buffer;
    }

    Vulkan_Buffer_Utils::Buffer_Allocation Upload_Context::Create_staging(VkDeviceSize _size)
    {
        // The slot is reserved before the buffer exists, so registering the
        // buffer cannot fail and leak it.
        staging_buffers.reserve(staging_buffers.size() + 1);

        const Vulkan_Buffer_Utils::Buffer_Allocation buffer = Vulkan_Buffer_Utils::Create_buffer(allocator, _size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);

        staging_buffers.push_back(buffer);

        return buffer;
    }

    void Upload_Context::Submit_and_wait(VkCommandBuffer _command_buffer)
    {
        if (_command_buffer == VK_NULL_HANDLE)
            throw std::invalid_argument("Upload_Context::Submit_and_wait: null command buffer");

        const VkDevice device_handle = device.Get_logical_device_handle();

        VK_CHECK(vkEndCommandBuffer(_command_buffer), "Upload_Context::Submit_and_wait: failed to end command buffer");

        // The fence is shared across transfers: unsignal it before reuse.
        const VkFence fence_handle = fence.Get();

        VK_CHECK(vkResetFences(device_handle, 1, &fence_handle), "Upload_Context::Submit_and_wait: reset transfer fence");

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &_command_buffer;

        VK_CHECK(vkQueueSubmit(device.Get_graphics_queue(), 1, &submit_info, fence_handle),
            "Upload_Context::Submit_and_wait: failed to submit");

        VK_CHECK(vkWaitForFences(device_handle, 1, &fence_handle, VK_TRUE, UINT64_MAX),
            "Upload_Context::Submit_and_wait: wait for transfer fence");
    }

    void Upload_Context::End(VkCommandBuffer _command_buffer)
    {
        Destroy_staging_buffers();
        command_pool.Free(_command_buffer);
    }

    void Upload_Context::Abort(VkCommandBuffer _command_buffer) noexcept
    {
        // The submission either never ran or was waited for by
        // Submit_and_wait before it threw; the idle wait also covers a
        // failure between the two.
        vkDeviceWaitIdle(device.Get_logical_device_handle());

        Destroy_staging_buffers();
        command_pool.Free(_command_buffer);
    }

} // namespace Renderer_System
