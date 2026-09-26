#include <Vulkan_Command_Pool.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer_System {

    // ---------- Constructor ----------
    Vulkan_Command_Pool::Vulkan_Command_Pool(const Vulkan_Device& _device,
        uint32_t _buffer_count,
        VkCommandPoolCreateFlags _flags)
        : device_handle(_device.Get_logical_device_handle()),
        command_pool(VK_NULL_HANDLE) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a command pool");

        const Queue_Family_Indices& indices = _device.Get_queue_family_indices();

        // ---------- Pool creation ----------
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = _flags;

        // Command buffers from this pool can only be submitted to queues
        // belonging to this specific family - we use the graphics family
        // since these buffers will record draw and transfer commands.
        pool_info.queueFamilyIndex = indices.graphics_family.value();

        VK_CHECK(vkCreateCommandPool(device_handle, &pool_info, nullptr, &command_pool),
            "Vulkan_Command_Pool: failed to create command pool");

        // ---------- Command buffer allocation ----------
        // The destructor does not run for a constructor that throws, so a
        // failure from here on destroys the pool created above. Destroy()
        // also clears command_buffers.
        try
        {
            if (_buffer_count > 0) {
                command_buffers.resize(_buffer_count);

                VkCommandBufferAllocateInfo alloc_info{};
                alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                alloc_info.commandPool = command_pool;

                // PRIMARY command buffers can be submitted directly to a queue.
                // SECONDARY buffers can only be called from within a primary
                // buffer (used for splitting recording work across threads) -
                // not needed for the current single-threaded recording approach.
                alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers.size());

                // Unlike most Vulkan creation calls, this one allocates MULTIPLE
                // command buffers in a single call, filling the whole vector at once.
                VK_CHECK(vkAllocateCommandBuffers(device_handle, &alloc_info, command_buffers.data()),
                    "Vulkan_Command_Pool: failed to allocate command buffers");
            }
        }
        catch (...)
        {
            Destroy();
            throw;
        }
    }

    // ---------- Destructor ----------
    Vulkan_Command_Pool::~Vulkan_Command_Pool() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Command_Pool::Destroy() {
        // Destroying the pool automatically frees every command buffer
        // allocated from it - calling vkFreeCommandBuffers individually
        // first is unnecessary (though not wrong, just redundant).
        if (command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_handle, command_pool, nullptr);
            command_pool = VK_NULL_HANDLE;
        }
        command_buffers.clear();
    }

    // ---------- Move constructor ----------
    Vulkan_Command_Pool::Vulkan_Command_Pool(Vulkan_Command_Pool&& _other) noexcept
        : device_handle(_other.device_handle),
        command_pool(_other.command_pool),
        command_buffers(std::move(_other.command_buffers)) {

        _other.command_pool = VK_NULL_HANDLE;
        _other.command_buffers.clear();
    }

    // ---------- Move assignment ----------
    Vulkan_Command_Pool& Vulkan_Command_Pool::operator=(Vulkan_Command_Pool&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            command_pool = _other.command_pool;
            command_buffers = std::move(_other.command_buffers);

            _other.command_pool = VK_NULL_HANDLE;
            _other.command_buffers.clear();
        }
        return *this;
    }

    // ---------- Get_command_buffer ----------
    VkCommandBuffer Vulkan_Command_Pool::Get_command_buffer(uint32_t _index) const {
        assert(command_pool != VK_NULL_HANDLE && "Get_command_buffer() called on a moved-from or destroyed Vulkan_Command_Pool");

        if (_index >= command_buffers.size())
            throw std::out_of_range("Vulkan_Command_Pool::Get_command_buffer: index out of range");

        return command_buffers[_index];
    }

    // ---------- Reset_command_buffer ----------
    void Vulkan_Command_Pool::Reset_command_buffer(uint32_t _index) const {
        assert(command_pool != VK_NULL_HANDLE && "Reset_command_buffer() called on a moved-from or destroyed Vulkan_Command_Pool");

        // 0 = no special reset flags (memory is kept for the next recording).
        VK_CHECK(vkResetCommandBuffer(Get_command_buffer(_index), 0),
            "Vulkan_Command_Pool: failed to reset command buffer");
    }

    // ---------- Allocate_primary ----------
    VkCommandBuffer Vulkan_Command_Pool::Allocate_primary() {
        assert(command_pool != VK_NULL_HANDLE && "Allocate_primary() called on a moved-from or destroyed Vulkan_Command_Pool");

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(device_handle, &alloc_info, &command_buffer),
            "Vulkan_Command_Pool: failed to allocate a one-off command buffer");

        return command_buffer;
    }

    // ---------- Free ----------
    void Vulkan_Command_Pool::Free(VkCommandBuffer _command_buffer) {
        if (_command_buffer == VK_NULL_HANDLE || command_pool == VK_NULL_HANDLE) return;

        vkFreeCommandBuffers(device_handle, command_pool, 1, &_command_buffer);
    }

    // ---------- Get_handle ----------
    VkCommandPool Vulkan_Command_Pool::Get_handle() const {
        assert(command_pool != VK_NULL_HANDLE && "Get_handle() called on a moved-from or destroyed Vulkan_Command_Pool");
        return command_pool;
    }

    // ---------- Get_buffer_count ----------
    uint32_t Vulkan_Command_Pool::Get_buffer_count() const {
        return static_cast<uint32_t>(command_buffers.size());
    }

}
