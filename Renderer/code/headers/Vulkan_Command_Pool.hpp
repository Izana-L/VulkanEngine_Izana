#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System {

    // Vulkan_Command_Pool: owns a VkCommandPool on the graphics queue
    // family and the primary VkCommandBuffers allocated from it.
    //
    // Two usage patterns, one class:
    //   - A per-frame pool: created with RESET_COMMAND_BUFFER_BIT and a
    //     fixed number of buffers that are reset and re-recorded every
    //     time their frame slot comes around (Frame_Data).
    //   - A transient pool: created with TRANSIENT_BIT and zero fixed
    //     buffers; Allocate_primary() / Free() hand out short-lived
    //     buffers for one-off uploads (Renderer::Upload_batch).
    // Both used to be written by hand at their call sites; this is the
    // one implementation of pool creation in the engine.
    class Vulkan_Command_Pool 
    {
        VkDevice device_handle;
        VkCommandPool command_pool;
        std::vector<VkCommandBuffer> command_buffers;

    public:
        // Creates the command pool (tied to the graphics queue family)
        // and allocates _buffer_count primary command buffers from it.
        // _flags: VkCommandPoolCreateFlags; RESET_COMMAND_BUFFER_BIT lets
        //   individual buffers be reset, TRANSIENT_BIT hints short-lived
        //   allocations.
        Vulkan_Command_Pool(const Vulkan_Device& _device,
            uint32_t _buffer_count,
            VkCommandPoolCreateFlags _flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

        ~Vulkan_Command_Pool();

        Vulkan_Command_Pool(const Vulkan_Command_Pool&) = delete;
        Vulkan_Command_Pool& operator=(const Vulkan_Command_Pool&) = delete;

        Vulkan_Command_Pool(Vulkan_Command_Pool&& _other) noexcept;
        Vulkan_Command_Pool& operator=(Vulkan_Command_Pool&& _other) noexcept;

        // Returns the command buffer assigned to a specific index
        // (0 to _buffer_count - 1). The caller is responsible for
        // resetting/recording it each time that index comes around again.
        VkCommandBuffer Get_command_buffer(uint32_t _index) const;

        // Resets a command buffer back to its initial state, ready to be
        // recorded into again. Requires RESET_COMMAND_BUFFER_BIT.
        void Reset_command_buffer(uint32_t _index) const;

        // Allocates one extra primary command buffer, not tracked by
        // Get_command_buffer(). The caller frees it with Free() once the
        // work it recorded has completed.
        VkCommandBuffer Allocate_primary();

        // Returns a buffer obtained from Allocate_primary() to the pool.
        void Free(VkCommandBuffer _command_buffer);

        // Raw pool handle.
        VkCommandPool Get_handle() const;

        // How many command buffers were allocated up front.
        uint32_t Get_buffer_count() const;

    private:
        // Destroys the command pool. Destroying the pool automatically
        // frees all command buffers allocated from it.
        void Destroy();
    };

}
