#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <MeshData.hpp>

#include <cstdint>

namespace Renderer
{

    // Mesh_GPU: GPU-resident geometry for a single mesh.
    //
    // Owns a vertex buffer and an index buffer uploaded from a
    // CoreTypes::MeshData. The upload is recorded into a caller-provided
    // command buffer (already open) — the caller is responsible for
    // submitting and waiting on that command buffer before using the mesh
    // for rendering.
    //
    // Because the copy hasn't happened yet when the constructor returns,
    // staging buffers are kept alive as members until the caller explicitly
    // releases them via Release_staging_buffers() after the submit+wait.
    //
    // Typical usage:
    //
    //   vkBeginCommandBuffer(cmd, ...);
    //   Mesh_GPU mesh(device, cmd, mesh_data);
    //   vkEndCommandBuffer(cmd);
    //   vkQueueSubmit(...);
    //   vkQueueWaitIdle(queue);
    //   mesh.Release_staging_buffers();   // staging memory freed here
    //
    //   // Now safe to use mesh for rendering:
    //   mesh.Bind(cmd);
    //   mesh.Draw(cmd);
    class Mesh_GPU
    {


        // =========================================================
        // Data
        // =========================================================

        VkDevice device_handle;

        // Final GPU-local buffers — used every frame for rendering.
        VkBuffer       vertex_buffer;
        VkDeviceMemory vertex_buffer_memory;

        VkBuffer       index_buffer;
        VkDeviceMemory index_buffer_memory;

        // Staging buffers — CPU-visible, used only during upload.
        // Kept alive until Release_staging_buffers() is called.
        VkBuffer       vertex_staging_buffer;
        VkDeviceMemory vertex_staging_memory;

        VkBuffer       index_staging_buffer;
        VkDeviceMemory index_staging_memory;

        uint32_t    vertex_count;
        uint32_t    index_count;
        VkIndexType index_type;     // translated from CoreTypes::Index_Type

    public:

        Mesh_GPU(
            const Vulkan_Device& _device,
            VkCommandBuffer            _transfer_cmd,
            const CoreTypes::MeshData& _mesh_data
        );

        ~Mesh_GPU();

        Mesh_GPU(const Mesh_GPU&) = delete;
        Mesh_GPU& operator=(const Mesh_GPU&) = delete;

        Mesh_GPU(Mesh_GPU&& _other) noexcept;
        Mesh_GPU& operator=(Mesh_GPU&& _other) noexcept;

        // =========================================================
        // Upload lifecycle
        // =========================================================

        // Frees the staging buffers used during upload.
        // Must be called after the transfer command buffer has been
        // submitted and the queue has finished (vkQueueWaitIdle or fence).
        // Calling this before the GPU has consumed the copy will corrupt
        // the upload — the assert in debug catches moved-from states but
        // not premature release.
        void Release_staging_buffers();

        // =========================================================
        // Render
        // =========================================================

        // Binds the vertex and index buffers into _command_buffer.
        // Must be called before Draw() within the same command buffer.
        void Bind(VkCommandBuffer _command_buffer) const;

        // Records an indexed draw call for the full mesh.
        // Bind() must have been called first in this command buffer.
        void Draw(VkCommandBuffer _command_buffer) const;

        // =========================================================
        // Query
        // =========================================================

        uint32_t Get_index_count()  const;
        uint32_t Get_vertex_count() const;

    private:

        void Destroy();

        
    };

} // namespace Renderer