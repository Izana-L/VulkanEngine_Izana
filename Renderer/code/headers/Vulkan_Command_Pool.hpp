#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <vector>

namespace Renderer_System {

    // Vulkan_Command_Pool: owns the VkCommandPool and a set of
    // VkCommandBuffer allocated from it, one per frame-in-flight.
    //
    // Command buffers are where drawing commands (vkCmdBeginRenderPass,
    // vkCmdDraw, etc.) are actually recorded before being submitted to a
    // queue. Having one buffer per frame-in-flight (rather than reusing a
    // single one) allows the CPU to record the next frame's commands
    // while the GPU is still processing the previous frame's buffer.
    class Vulkan_Command_Pool 
    {
        VkDevice device_handle;
        VkCommandPool command_pool;
        std::vector<VkCommandBuffer> command_buffers;

    public:
        // Creates the command pool (tied to the graphics queue family)
        // and allocates _max_frames_in_flight command buffers from it.
        Vulkan_Command_Pool(const Vulkan_Device& _device, uint32_t _max_frames_in_flight);

        ~Vulkan_Command_Pool();

        Vulkan_Command_Pool(const Vulkan_Command_Pool&) = delete;
        Vulkan_Command_Pool& operator=(const Vulkan_Command_Pool&) = delete;

        Vulkan_Command_Pool(Vulkan_Command_Pool&& _other) noexcept;
        Vulkan_Command_Pool& operator=(Vulkan_Command_Pool&& _other) noexcept;

        // Returns the command buffer assigned to a specific frame-in-flight
        // index (0 to _max_frames_in_flight - 1). The caller is
        // responsible for resetting/recording it each time that frame
        // index comes around again.
        VkCommandBuffer Get_command_buffer(uint32_t _frame_index) const;

        // Resets a command buffer back to its initial state, ready to be
        // recorded into again. Must be called before recording new
        // commands into a buffer that was already used in a previous frame.
        void Reset_command_buffer(uint32_t _frame_index) const;

        // Raw pool handle - rarely needed externally, but exposed in case
        // some operation requires it directly (e.g. allocating a one-off
        // command buffer for a resource upload, later).
        VkCommandPool Get_handle() const;

        // How many command buffers were allocated (matches max_frames_in_flight)
        uint32_t Get_buffer_count() const;

    private:
        // Destroys the command pool. Note: destroying the pool
        // automatically frees all command buffers allocated from it - we
        // don't need to free them individually.
        void Destroy();

       
    };

}