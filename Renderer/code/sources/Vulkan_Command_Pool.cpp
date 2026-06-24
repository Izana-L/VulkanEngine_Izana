#include <Vulkan_Command_Pool.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer {

    // ---------- Constructor ----------
    Vulkan_Command_Pool::Vulkan_Command_Pool(const Vulkan_Device& _device, uint32_t _max_frames_in_flight)
        : device_handle(_device.Get_logical_device_handle()),
        command_pool(VK_NULL_HANDLE) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a command pool");
        assert(_max_frames_in_flight >= 1 && "_max_frames_in_flight must be at least 1");

        const Queue_Family_Indices& indices = _device.Get_queue_family_indices();

        // ---------- Pool creation ----------
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;

        // RESET_COMMAND_BUFFER_BIT allows individual command buffers from
        // this pool to be reset independently (vkResetCommandBuffer),
        // rather than having to reset the entire pool at once. This is
        // what lets us reset/re-record just one frame's buffer per frame.
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        // Command buffers from this pool can only be submitted to queues
        // belonging to this specific family - we use the graphics family
        // since these buffers will record draw commands.
        pool_info.queueFamilyIndex = indices.graphics_family.value();

        VkResult result = vkCreateCommandPool(device_handle, &pool_info, nullptr, &command_pool);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create command pool: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        // ---------- Command buffer allocation ----------
        command_buffers.resize(_max_frames_in_flight);

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool;

        // PRIMARY command buffers can be submitted directly to a queue.
        // SECONDARY buffers can only be called from within a primary
        // buffer (used for splitting recording work across threads) -
        // not needed for our current single-threaded recording approach.
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers.size());

        // Unlike most Vulkan creation calls, this one allocates MULTIPLE
        // command buffers in a single call, filling the whole vector at once.
        result = vkAllocateCommandBuffers(device_handle, &alloc_info, command_buffers.data());
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to allocate command buffers: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        std::cout << "[Vulkan_Command_Pool] Created command pool with " << command_buffers.size()
            << " command buffer(s).\n";
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
    VkCommandBuffer Vulkan_Command_Pool::Get_command_buffer(uint32_t _frame_index) const {
        assert(command_pool != VK_NULL_HANDLE && "Get_command_buffer() called on a moved-from or destroyed Vulkan_Command_Pool");
        assert(_frame_index < command_buffers.size() && "Get_command_buffer() called with an out-of-range frame index");

        return command_buffers[_frame_index];
    }

    // ---------- Reset_command_buffer ----------
    void Vulkan_Command_Pool::Reset_command_buffer(uint32_t _frame_index) const {
        assert(command_pool != VK_NULL_HANDLE && "Reset_command_buffer() called on a moved-from or destroyed Vulkan_Command_Pool");
        assert(_frame_index < command_buffers.size() && "Reset_command_buffer() called with an out-of-range frame index");

        // 0 = no special reset flags (we don't need to release memory
        // back to the pool, just reset the buffer to be recorded again)
        vkResetCommandBuffer(command_buffers[_frame_index], 0);
    }

    // ---------- Get_handle ----------
    VkCommandPool Vulkan_Command_Pool::Get_handle() const {
        assert(command_pool != VK_NULL_HANDLE && "Get_handle() called on a moved-from or destroyed Vulkan_Command_Pool");
        return command_pool;
    }

    // ---------- Get_buffer_count ----------
    uint32_t Vulkan_Command_Pool::Get_buffer_count() const {
        assert(command_pool != VK_NULL_HANDLE && "Get_buffer_count() called on a moved-from or destroyed Vulkan_Command_Pool");
        return static_cast<uint32_t>(command_buffers.size());
    }

}