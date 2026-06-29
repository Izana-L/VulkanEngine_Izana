#include <Renderer.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <glm/glm.hpp>

#include <stdexcept>
#include <iostream>
#include <cassert>
#include <cstring>
#include <array>

namespace Renderer
{

    // =========================================================
    // Constructor
    // =========================================================

    Renderer::Renderer(const Platform::Window& _window, bool _enable_validation)

        // Construction order mirrors member declaration order.
        // Each object depends on the ones above it.
        : instance(_enable_validation, "Game", "Engine"),
        surface(instance, _window),
        device(instance, surface),
        swapchain(device, surface, _window, 3, false),
        render_pass(device,swapchain.Get_image_format(),device.Find_supported_depth_format()),
        depth_resources(device,device.Find_supported_depth_format(),swapchain.Get_extent()),
        framebuffers(device, render_pass, swapchain, depth_resources),
        pipeline(device, render_pass, []
            {
                Pipeline_Config config;
                config.vertex_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.vert.spv";
                config.fragment_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.frag.spv";
                return config;
            }()),
        descriptor_pool(VK_NULL_HANDLE),
        transfer_command_pool(VK_NULL_HANDLE)
    {
        // ── Frame resources ────────────────────────────────────────
        for (auto& frame : frames)
            frame.Init(device);

        // ── Descriptor pool + sets ─────────────────────────────────
        Init_descriptor_pool();
        Init_descriptor_sets();

        // ── Transfer command pool ──────────────────────────────────
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex =
            device.Get_queue_family_indices().graphics_family.value();

        VkResult result = vkCreateCommandPool(
            device.Get_logical_device_handle(), &pool_info, nullptr, &transfer_command_pool);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Renderer: failed to create transfer command pool: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        std::cout << "[Renderer] Initialized.\n";
    }

    // =========================================================
    // Destructor
    // =========================================================

    Renderer::~Renderer()
    {
        // Wait for the GPU to finish before destroying anything.
        vkDeviceWaitIdle(device.Get_logical_device_handle());

        // Meshes first — they hold GPU buffers.
        meshes.clear();

        if (transfer_command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device.Get_logical_device_handle(),
                transfer_command_pool, nullptr);

        if (descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device.Get_logical_device_handle(),
                descriptor_pool, nullptr);

        for (auto& frame : frames)
            frame.Destroy(device.Get_logical_device_handle());

        // Vulkan core objects are destroyed in reverse construction
        // order by their own destructors (RAII).
        std::cout << "[Renderer] Destroyed.\n";
    }

    // =========================================================
    // Upload_mesh
    // =========================================================

    uint32_t Renderer::Upload_mesh(const CoreTypes::MeshData& _mesh_data)
    {
        assert(!_mesh_data.vertices.empty() && "Upload_mesh: MeshData has no vertices");
        assert(!_mesh_data.indices.empty() && "Upload_mesh: MeshData has no indices");

        VkDevice device_handle = device.Get_logical_device_handle();

        // Allocate a one-off transfer command buffer.
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = transfer_command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer transfer_cmd = VK_NULL_HANDLE;
        VkResult result = vkAllocateCommandBuffers(device_handle, &alloc_info, &transfer_cmd);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Upload_mesh: failed to allocate transfer command buffer: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        // Begin recording.
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(transfer_cmd, &begin_info);

        // Mesh_GPU records vkCmdCopyBuffer into transfer_cmd.
        // Staging buffers stay alive inside the Mesh_GPU until we
        // call Release_staging_buffers() below.
        meshes.emplace_back(device, transfer_cmd, _mesh_data);

        vkEndCommandBuffer(transfer_cmd);

        // Submit and wait — acceptable for load-time uploads.
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &transfer_cmd;

        vkQueueSubmit(device.Get_graphics_queue(), 1, &submit_info, VK_NULL_HANDLE);
        vkQueueWaitIdle(device.Get_graphics_queue());

        // GPU has consumed the staging data — safe to free.
        meshes.back().Release_staging_buffers();

        vkFreeCommandBuffers(device_handle, transfer_command_pool, 1, &transfer_cmd);

        const uint32_t gpu_id = static_cast<uint32_t>(meshes.size() - 1);
        std::cout << "[Renderer] Mesh uploaded, gpu_id = " << gpu_id << "\n";
        return gpu_id;
    }

    // =========================================================
    // Render
    // =========================================================

    void Renderer::Render(const CoreTypes::RenderPacket& _packet)
    {
        Frame_Data& frame = frames[current_frame];
        VkDevice    dev = device.Get_logical_device_handle();

        // ── Wait for this frame slot to be free ───────────────────
        vkWaitForFences(dev, 1, &frame.in_flight_fence, VK_TRUE, UINT64_MAX);

        // ── Wait for this slot's previous present to complete ─────
        // With swapchain_maintenance1, the present operation signals
        // present_fence when done. We must wait on it before reusing
        // render_finished_semaphore (which the present waits on).
        // present_fence_pending guards the first-frame case where no
        // present has happened yet for this slot.
        if (device.Is_swapchain_maintenance1_enabled() && frame.present_fence_pending)
        {
            vkWaitForFences(dev, 1, &frame.present_fence, VK_TRUE, UINT64_MAX);
            vkResetFences(dev, 1, &frame.present_fence);
            frame.present_fence_pending = false;
        }

        // ── Acquire swapchain image ───────────────────────────────
        uint32_t image_index = 0;
        VkResult result = vkAcquireNextImageKHR(
            dev,
            swapchain.Get_handle(),
            UINT64_MAX,
            frame.image_available_semaphore,
            VK_NULL_HANDLE,
            &image_index
        );

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            Recreate_swapchain();
            return;  // skip this frame, retry next
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error(
                "Render: failed to acquire swapchain image: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        // ── Update uniform buffer ─────────────────────────────────
        Frame_UBO ubo{};
        ubo.view = _packet.view.view;
        ubo.projection = _packet.view.projection;
        std::memcpy(frame.uniform_mapped_ptr, &ubo, sizeof(ubo));

        // ── Reset fence just before submit (not before acquire) ───
        vkResetFences(dev, 1, &frame.in_flight_fence);

        // ── Record commands ───────────────────────────────────────
        vkResetCommandBuffer(frame.command_buffer, 0);
        Record_command_buffer(_packet, image_index);

        // ── Submit ────────────────────────────────────────────────
        VkPipelineStageFlags wait_stages[] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        };

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &frame.image_available_semaphore;
        submit_info.pWaitDstStageMask = wait_stages;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &frame.command_buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &frame.render_finished_semaphore;

        result = vkQueueSubmit(
            device.Get_graphics_queue(), 1, &submit_info, frame.in_flight_fence);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Render: failed to submit command buffer: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        // ── Present ───────────────────────────────────────────────
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &frame.render_finished_semaphore;
        present_info.swapchainCount = 1;
        VkSwapchainKHR swapchain_handle = swapchain.Get_handle();
        present_info.pSwapchains = &swapchain_handle;
        present_info.pImageIndices = &image_index;

        // Attach a present fence so we know when the present completes and
        // can safely reuse render_finished_semaphore next time this slot
        // comes around (VK_KHR_swapchain_maintenance1).
        VkSwapchainPresentFenceInfoEXT present_fence_info{};
        if (device.Is_swapchain_maintenance1_enabled())
        {
            present_fence_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
            present_fence_info.swapchainCount = 1;
            present_fence_info.pFences        = &frame.present_fence;
            present_info.pNext = &present_fence_info;

            frame.present_fence_pending = true;
        }

        result = vkQueuePresentKHR(device.Get_present_queue(), &present_info);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            Recreate_swapchain();
        else if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Render: failed to present: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        current_frame = (current_frame + 1) % FRAMES_IN_FLIGHT;
    }

    // =========================================================
    // Record_command_buffer
    // =========================================================

    void Renderer::Record_command_buffer(
        const CoreTypes::RenderPacket& _packet,
        uint32_t                       _image_index)
    {
        Frame_Data& frame = frames[current_frame];

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        vkBeginCommandBuffer(frame.command_buffer, &begin_info);

        // ── Render pass ───────────────────────────────────────────
        std::array<VkClearValue, 2> clear_values{};
        clear_values[0].color = { { 0.01f, 0.01f, 0.01f, 1.0f } };
        clear_values[1].depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_info.renderPass = render_pass.Get_handle();
        render_pass_info.framebuffer = framebuffers.Get_framebuffer(_image_index);
        render_pass_info.renderArea.offset = { 0, 0 };
        render_pass_info.renderArea.extent = swapchain.Get_extent();
        render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
        render_pass_info.pClearValues = clear_values.data();

        vkCmdBeginRenderPass(frame.command_buffer,
            &render_pass_info,
            VK_SUBPASS_CONTENTS_INLINE);

        // ── Bind pipeline ─────────────────────────────────────────
        vkCmdBindPipeline(frame.command_buffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline.Get_handle());

        // ── Dynamic viewport + scissor ────────────────────────────
        VkExtent2D extent = swapchain.Get_extent();

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(frame.command_buffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = extent;
        vkCmdSetScissor(frame.command_buffer, 0, 1, &scissor);

        // ── Bind descriptor set (VP matrices) ────────────────────
        vkCmdBindDescriptorSets(
            frame.command_buffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline.Get_layout_handle(),
            0, 1,
            &descriptor_sets[current_frame],
            0, nullptr
        );

        // ── Draw opaque items ─────────────────────────────────────
        for (const CoreTypes::Draw_Item& item : _packet.opaque_items)
        {
            if (item.mesh_gpu_id >= meshes.size()) continue;

            // Push this item's model matrix to the vertex shader.
            // transform_idx indexes into the packet's transform array,
            // which the Extractor filled with world matrices.
            assert(item.transform_idx < _packet.transform_count &&
                "Record_command_buffer: transform_idx out of range");

            const glm::mat4& model = _packet.transforms[item.transform_idx];
            vkCmdPushConstants(
                frame.command_buffer,
                pipeline.Get_layout_handle(),
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(glm::mat4),
                &model
            );

            Mesh_GPU& mesh = meshes[item.mesh_gpu_id];
            mesh.Bind(frame.command_buffer);
            mesh.Draw(frame.command_buffer);
        }

        vkCmdEndRenderPass(frame.command_buffer);
        vkEndCommandBuffer(frame.command_buffer);
    }

    // =========================================================
    // Recreate_swapchain
    // =========================================================

    void Renderer::Recreate_swapchain()
    {
        vkDeviceWaitIdle(device.Get_logical_device_handle());

        // After waiting idle, any pending present operations are done.
        // Reset the present fences and their pending flags so the next
        // frames don't wait on a fence that will never be signaled by an
        // operation that belonged to the old swapchain.
        if (device.Is_swapchain_maintenance1_enabled())
        {
            VkDevice dev = device.Get_logical_device_handle();
            for (auto& frame : frames)
            {
                if (frame.present_fence_pending)
                {
                    vkResetFences(dev, 1, &frame.present_fence);
                    frame.present_fence_pending = false;
                }
            }
        }

        swapchain.Recreate();
        depth_resources.Recreate(swapchain.Get_extent());
        framebuffers.Recreate(render_pass, swapchain, depth_resources);

        std::cout << "[Renderer] Swapchain recreated.\n";
    }

    // =========================================================
    // Init_descriptor_pool
    // =========================================================

    void Renderer::Init_descriptor_pool()
    {
        // One uniform buffer descriptor per frame-in-flight.
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_size.descriptorCount = FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        pool_info.maxSets = FRAMES_IN_FLIGHT;

        VkResult result = vkCreateDescriptorPool(
            device.Get_logical_device_handle(), &pool_info, nullptr, &descriptor_pool);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Renderer: failed to create descriptor pool: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }
    }

    // =========================================================
    // Init_descriptor_sets
    // =========================================================

    void Renderer::Init_descriptor_sets()
    {
        // Allocate one descriptor set per frame from the same layout.
        std::array<VkDescriptorSetLayout, FRAMES_IN_FLIGHT> layouts;
        layouts.fill(pipeline.Get_descriptor_set_layout());

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = FRAMES_IN_FLIGHT;
        alloc_info.pSetLayouts = layouts.data();

        VkResult result = vkAllocateDescriptorSets(
            device.Get_logical_device_handle(), &alloc_info, descriptor_sets.data());

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Renderer: failed to allocate descriptor sets: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        // Point each descriptor set at the uniform buffer of its frame slot.
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorBufferInfo buffer_info{};
            buffer_info.buffer = frames[i].uniform_buffer;
            buffer_info.offset = 0;
            buffer_info.range = sizeof(Frame_UBO);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptor_sets[i];
            write.dstBinding = 0;
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &buffer_info;

            vkUpdateDescriptorSets(
                device.Get_logical_device_handle(), 1, &write, 0, nullptr);
        }
    }

} // namespace Renderer