#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Instance.hpp>
#include <Vulkan_Surface.hpp>
#include <Vulkan_Device.hpp>
#include <Vulkan_Swapchain.hpp>
#include <Vulkan_Render_Pass.hpp>
#include <Vulkan_Depth_Resources.hpp>
#include <Vulkan_Framebuffer.hpp>
#include <Vulkan_Pipeline.hpp>
#include <Vulkan_Command_Pool.hpp>
#include <Vulkan_Sync.hpp>
#include <Vertex.hpp>
#include <Window.hpp>

#include <vector>
#include <memory>

namespace Renderer 
{

    // Renderer: orchestrates every Vulkan_* class into a working render
    // loop. Owns all Vulkan resources, exposes a simple Initialize() /
    // Draw_frame() / Shutdown() API to the rest of the engine, and
    // handles swapchain recreation when the window is resized.
    //
    // For now, draws a single hardcoded triangle using a real vertex
    // buffer and an MVP uniform buffer (currently set to identity
    // matrices - camera/transform support comes later).
    class Renderer 
    {
        Platform::Window window;

        Vulkan_Instance instance;
        Vulkan_Surface surface;
        Vulkan_Device device;
        Vulkan_Swapchain swapchain;
        Vulkan_Render_Pass render_pass;
        Vulkan_Depth_Resources depth_resources;
        Vulkan_Framebuffer framebuffer;
        Vulkan_Pipeline pipeline;
        Vulkan_Command_Pool command_pool;
        Vulkan_Sync sync;

        // Vertex buffer for the hardcoded triangle
        VkBuffer vertex_buffer;
        VkDeviceMemory vertex_buffer_memory;
        uint32_t vertex_count;

        // One uniform buffer per frame-in-flight, plus a persistently
        // mapped pointer to each so we can write to it without
        // map/unmap overhead every frame.
        std::vector<VkBuffer> uniform_buffers;
        std::vector<VkDeviceMemory> uniform_buffers_memory;
        std::vector<void*> uniform_buffers_mapped;

        VkDescriptorPool descriptor_pool;
        std::vector<VkDescriptorSet> descriptor_sets;

        uint32_t current_frame;

    public:
        // Creates the window and initializes the entire Vulkan pipeline:
        // instance, surface, device, swapchain, render pass, depth
        // resources, framebuffers, pipeline, command pool, sync objects,
        // and uploads the triangle's vertex data.
        Renderer(uint32_t _window_width, uint32_t _window_height, const std::string& _window_title);

        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        Renderer(Renderer&&) = delete;
        Renderer& operator=(Renderer&&) = delete;

        // Renders and presents a single frame. Call this once per
        // iteration of the main loop. Handles waiting on the
        // frame-in-flight fence, acquiring a swapchain image, recording
        // and submitting the command buffer, presenting, and recreating
        // the swapchain if needed (resize, or out-of-date/suboptimal results).
        void Draw_frame();

        // Returns true if the user requested the window to close - use
        // this as the main loop's exit condition.
        bool Should_close() const;

        // Processes pending window events. Call once per frame, typically
        // right before or after Draw_frame().
        void Poll_events();

        // Blocks until the GPU has finished all submitted work. Call
        // this before destroying the Renderer (or any resources it
        // owns), to make sure the GPU isn't still using them.
        void Wait_idle() const;

        // Access to the underlying window, in case other systems need it
        // (e.g. Input, later).
        Platform::Window& Get_window();

    private:
        // Recreates the swapchain, depth resources, and framebuffers -
        // called when the window is resized or when Acquire/Present
        // report the swapchain is out of date.
        void Recreate_swapchain();

        // Records all drawing commands for one frame into the given
        // command buffer, targeting the given framebuffer/image index.
        void Record_command_buffer(VkCommandBuffer _command_buffer, uint32_t _image_index);

        // Creates the vertex buffer for the (currently hardcoded)
        // triangle, using the staging-buffer pattern via Vulkan_Buffer_Utils.
        void Create_vertex_buffer();

        // Creates one uniform buffer per frame-in-flight, used to upload
        // Model/View/Projection matrices to the vertex shader.
        void Create_uniform_buffers();

        // Creates the descriptor pool and allocates one descriptor set
        // per frame-in-flight, each pointing at that frame's uniform buffer.
        void Create_descriptor_pool_and_sets();

        // Updates the uniform buffer for the given frame-in-flight index
        // with the current MVP matrices. Currently uses identity matrices
        // (no camera/transform system yet).
        void Update_uniform_buffer(uint32_t _frame_index);

        static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
        static constexpr uint32_t SWAPCHAIN_IMAGE_COUNT = 3;

        
    };

}