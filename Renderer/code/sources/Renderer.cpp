#include <Renderer.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Utils.hpp>
#include <Filesystem.hpp>
#include <Matrix4.hpp>
#include <stdexcept>
#include <iostream>
#include <cassert>
#include <cstring>

namespace Renderer {

    // ---------- Constructor ----------
    Renderer::Renderer(uint32_t _window_width, uint32_t _window_height, const std::string& _window_title)
        : window(_window_width, _window_height, _window_title),
        instance(
#ifdef _DEBUG
            true
#else
            false
#endif
        ),
        surface(instance, window),
        device(instance, surface),
        swapchain(device, surface, window, SWAPCHAIN_IMAGE_COUNT, true), // true = prefer MAILBOX
        render_pass(device, swapchain.Get_image_format(), device.Find_supported_depth_format()),
        depth_resources(device, device.Find_supported_depth_format(), swapchain.Get_extent()),
        framebuffer(device, render_pass, swapchain, depth_resources),
        pipeline(device,render_pass,
            Platform::Filesystem::Combine_path("..\\..\\Renderer\\shaders\\compiled", "triangle.vert.spv"),
            Platform::Filesystem::Combine_path("..\\..\\Renderer\\shaders\\compiled", "triangle.frag.spv")),
        command_pool(device, MAX_FRAMES_IN_FLIGHT),
        sync(device, MAX_FRAMES_IN_FLIGHT, SWAPCHAIN_IMAGE_COUNT, device.Is_swapchain_maintenance1_enabled()),
        vertex_buffer(VK_NULL_HANDLE),
        vertex_buffer_memory(VK_NULL_HANDLE),
        vertex_count(0),
        descriptor_pool(VK_NULL_HANDLE),
        current_frame(0) {

        Create_vertex_buffer();
        Create_uniform_buffers();
        Create_descriptor_pool_and_sets();

        std::cout << "[Renderer] Initialization complete.\n";
    }

    // ---------- Destructor ----------
    Renderer::~Renderer() {
        // Make sure the GPU is done with everything before destroying
        // any resources - without this, we could destroy buffers/images
        // the GPU is still actively using, causing a crash or corruption.
        Wait_idle();

        VkDevice device_handle = device.Get_logical_device_handle();

        if (descriptor_pool != VK_NULL_HANDLE) {
            // Destroying the pool automatically frees the descriptor sets
            // allocated from it - no need to free them individually.
            vkDestroyDescriptorPool(device_handle, descriptor_pool, nullptr);
        }

        for (size_t i = 0; i < uniform_buffers.size(); ++i) {
            vkDestroyBuffer(device_handle, uniform_buffers[i], nullptr);
            vkFreeMemory(device_handle, uniform_buffers_memory[i], nullptr);
        }

        if (vertex_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_handle, vertex_buffer, nullptr);
            vkFreeMemory(device_handle, vertex_buffer_memory, nullptr);
        }

        // All other members (sync, command_pool, pipeline, framebuffer,
        // depth_resources, render_pass, swapchain, device, surface,
        // instance, window) clean themselves up automatically via their
        // own destructors, in reverse declaration order - this is why
        // member declaration order in the header matters.
    }

    // ---------- Should_close ----------
    bool Renderer::Should_close() const {
        return window.Should_close();
    }

    // ---------- Poll_events ----------
    void Renderer::Poll_events() {
        window.Poll_events();
    }

    // ---------- Wait_idle ----------
    void Renderer::Wait_idle() const {
        vkDeviceWaitIdle(device.Get_logical_device_handle());
    }

    // ---------- Get_window ----------
    Platform::Window& Renderer::Get_window() {
        return window;
    }

    // ---------- Draw_frame ----------
    void Renderer::Draw_frame() {
        if (window.Is_minimized()) return;

        sync.Wait_for_fence(current_frame);

        uint32_t image_index = 0;

        // Before acquiring, wait for the present fence of the slot we're
        // about to reuse - this guarantees the presentation engine has
        // finished with that image's semaphore before we signal it again.
        if (sync.Has_present_fences()) 
        {
            sync.Wait_and_reset_present_fence(current_frame % SWAPCHAIN_IMAGE_COUNT);
        }

        bool acquired = swapchain.Acquire_next_image(
            sync.Get_image_available_semaphore(current_frame % SWAPCHAIN_IMAGE_COUNT),
            image_index
        );

        if (!acquired) {
            Recreate_swapchain();
            return;
        }

        sync.Reset_fence(current_frame);
        Update_uniform_buffer(current_frame);

        command_pool.Reset_command_buffer(current_frame);
        VkCommandBuffer command_buffer = command_pool.Get_command_buffer(current_frame);
        Record_command_buffer(command_buffer, image_index);

        VkSemaphore wait_semaphore = sync.Get_image_available_semaphore(current_frame % SWAPCHAIN_IMAGE_COUNT);
        VkSemaphore signal_semaphore = sync.Get_render_finished_semaphore(current_frame);
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &wait_semaphore;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &signal_semaphore;

        VkResult result = vkQueueSubmit(
            device.Get_graphics_queue(), 1, &submit_info,
            sync.Get_in_flight_fence(current_frame)
        );
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to submit draw command buffer: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        // Pass the present fence if available - the presentation engine
        // will signal it when it's done with this image's semaphore.
        VkFence present_fence = sync.Has_present_fences()
            ? sync.Get_present_fence(current_frame % SWAPCHAIN_IMAGE_COUNT)
            : VK_NULL_HANDLE;

        bool presented = swapchain.Present(signal_semaphore, image_index, present_fence);
        if (!presented) Recreate_swapchain();

        current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    // ---------- Recreate_swapchain ----------
    void Renderer::Recreate_swapchain() {
        Wait_idle();

        swapchain.Recreate();
        depth_resources.Recreate(swapchain.Get_extent());
        framebuffer.Recreate(render_pass, swapchain, depth_resources);

        std::cout << "[Renderer] Swapchain, depth resources, and framebuffers recreated.\n";
    }

    // ---------- Record_command_buffer ----------
    void Renderer::Record_command_buffer(VkCommandBuffer _command_buffer, uint32_t _image_index) {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        VkResult result = vkBeginCommandBuffer(_command_buffer, &begin_info);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to begin recording command buffer: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        // ---------- Begin render pass ----------
        VkExtent2D extent = swapchain.Get_extent();

        // clearValues[0] = color clear (dark gray background)
        // clearValues[1] = depth clear (1.0 = farthest possible depth)
        // The order MUST match the attachment order defined in
        // Vulkan_Render_Pass (color at index 0, depth at index 1).
        std::array<VkClearValue, 2> clear_values{};
        clear_values[0].color = { {0.05f, 0.05f, 0.08f, 1.0f} };
        clear_values[1].depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_info.renderPass = render_pass.Get_handle();
        render_pass_info.framebuffer = framebuffer.Get_framebuffer(_image_index);
        render_pass_info.renderArea.offset = { 0, 0 };
        render_pass_info.renderArea.extent = extent;
        render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
        render_pass_info.pClearValues = clear_values.data();

        vkCmdBeginRenderPass(_command_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get_handle());

        // ---------- Dynamic viewport/scissor ----------
        // Set every frame since the pipeline was built with these as
        // dynamic state - this is what lets us avoid recreating the
        // pipeline on every window resize.
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(_command_buffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = extent;
        vkCmdSetScissor(_command_buffer, 0, 1, &scissor);

        // ---------- Bind vertex buffer and descriptor set ----------
        VkBuffer vertex_buffers[] = { vertex_buffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(_command_buffer, 0, 1, vertex_buffers, offsets);

        vkCmdBindDescriptorSets(
            _command_buffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline.Get_layout_handle(),
            0, 1, &descriptor_sets[current_frame],
            0, nullptr
        );

        // ---------- Draw ----------
        vkCmdDraw(_command_buffer, vertex_count, 1, 0, 0);

        vkCmdEndRenderPass(_command_buffer);

        result = vkEndCommandBuffer(_command_buffer);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to end recording command buffer: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }
    }

    // ---------- Create_vertex_buffer ----------
    void Renderer::Create_vertex_buffer() {
        // Hardcoded triangle for now - same shape/colors as the original
        // shader-only version, but now as real vertex data.
        std::vector<Vertex_Static_Mesh> vertices = {
            { {  0.0f, -0.5f, 0.0f }, {0,0,1}, {1,0,0,1}, {0.5f, 0.0f}, {1,0,0,1} },
            { {  0.5f,  0.5f, 0.0f }, {0,0,1}, {1,0,0,1}, {1.0f, 1.0f}, {0,1,0,1} },
            { { -0.5f,  0.5f, 0.0f }, {0,0,1}, {1,0,0,1}, {0.0f, 1.0f}, {0,0,1,1} },
        };

        vertex_count = static_cast<uint32_t>(vertices.size());
        VkDeviceSize buffer_size = sizeof(Vertex_Static_Mesh) * vertices.size();

        // ---------- Staging buffer (CPU-writable) ----------
        VkBuffer staging_buffer;
        VkDeviceMemory staging_buffer_memory;

        Vulkan_Buffer_Utils::Create_buffer(
            device,
            buffer_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging_buffer,
            staging_buffer_memory
        );

        // Map the staging buffer's memory into CPU address space, copy
        // the vertex data into it, then unmap - HOST_COHERENT_BIT means
        // we don't need to manually flush the write for the GPU to see it.
        void* data;
        vkMapMemory(device.Get_logical_device_handle(), staging_buffer_memory, 0, buffer_size, 0, &data);
        std::memcpy(data, vertices.data(), static_cast<size_t>(buffer_size));
        vkUnmapMemory(device.Get_logical_device_handle(), staging_buffer_memory);

        // ---------- Real vertex buffer (GPU-only, fast) ----------
        Vulkan_Buffer_Utils::Create_buffer(
            device,
            buffer_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            vertex_buffer,
            vertex_buffer_memory
        );

        Vulkan_Buffer_Utils::Copy_buffer(
            device,
            command_pool.Get_handle(),
            staging_buffer,
            vertex_buffer,
            buffer_size
        );

        // Staging buffer is only needed for the upload - safe to destroy now.
        vkDestroyBuffer(device.Get_logical_device_handle(), staging_buffer, nullptr);
        vkFreeMemory(device.Get_logical_device_handle(), staging_buffer_memory, nullptr);

        std::cout << "[Renderer] Vertex buffer created (" << vertex_count << " vertices).\n";
    }

    // ---------- Create_uniform_buffers ----------
    void Renderer::Create_uniform_buffers() {
        // A simple placeholder MVP layout for now - just three 4x4
        // matrices. A dedicated struct will likely replace this once
        // camera/transform systems exist.
        VkDeviceSize buffer_size = sizeof(MathLib::Matrix4) * 3; // model, view, projection

        uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
        uniform_buffers_memory.resize(MAX_FRAMES_IN_FLIGHT);
        uniform_buffers_mapped.resize(MAX_FRAMES_IN_FLIGHT);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            Vulkan_Buffer_Utils::Create_buffer(
                device,
                buffer_size,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                uniform_buffers[i],
                uniform_buffers_memory[i]
            );

            // Persistently mapped: we map once here and keep writing to
            // this pointer every frame, instead of map/unmap-ing each
            // time - cheaper since uniform buffers are written every frame.
            vkMapMemory(device.Get_logical_device_handle(), uniform_buffers_memory[i], 0, buffer_size, 0, &uniform_buffers_mapped[i]);
        }

        std::cout << "[Renderer] Uniform buffers created (" << MAX_FRAMES_IN_FLIGHT << " buffers).\n";
    }

    // ---------- Create_descriptor_pool_and_sets ----------
    void Renderer::Create_descriptor_pool_and_sets() {
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        pool_info.maxSets = MAX_FRAMES_IN_FLIGHT;

        VkResult result = vkCreateDescriptorPool(device.Get_logical_device_handle(), &pool_info, nullptr, &descriptor_pool);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create descriptor pool: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        // We need one copy of the same layout per frame-in-flight, since
        // vkAllocateDescriptorSets expects one layout per set being allocated.
        std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, pipeline.Get_descriptor_set_layout());

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        alloc_info.pSetLayouts = layouts.data();

        descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
        result = vkAllocateDescriptorSets(device.Get_logical_device_handle(), &alloc_info, descriptor_sets.data());
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to allocate descriptor sets: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        // Point each descriptor set at its corresponding frame's uniform buffer.
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VkDescriptorBufferInfo buffer_info{};
            buffer_info.buffer = uniform_buffers[i];
            buffer_info.offset = 0;
            buffer_info.range = sizeof(MathLib::Matrix4) * 3;

            VkWriteDescriptorSet descriptor_write{};
            descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptor_write.dstSet = descriptor_sets[i];
            descriptor_write.dstBinding = 0; // matches binding = 0 in Vulkan_Pipeline's descriptor set layout
            descriptor_write.dstArrayElement = 0;
            descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptor_write.descriptorCount = 1;
            descriptor_write.pBufferInfo = &buffer_info;

            vkUpdateDescriptorSets(device.Get_logical_device_handle(), 1, &descriptor_write, 0, nullptr);
        }

        std::cout << "[Renderer] Descriptor pool and sets created.\n";
    }

    // ---------- Update_uniform_buffer ----------
    void Renderer::Update_uniform_buffer(uint32_t _frame_index) {
        // Identity matrices for now - the triangle is drawn directly in
        // clip space with no actual transformation yet. This will be
        // replaced once a camera/transform system exists.
        struct MVP {
            MathLib::Matrix4 model;
            MathLib::Matrix4 view;
            MathLib::Matrix4 projection;
        };

        MVP mvp{};
        mvp.model = MathLib::Mat4::Identity();
        mvp.view = MathLib::Mat4::Identity();
        mvp.projection = MathLib::Mat4::Identity();

        std::memcpy(uniform_buffers_mapped[_frame_index], &mvp, sizeof(mvp));
    }

}