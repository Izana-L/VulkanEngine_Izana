#include <Mesh_GPU.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <cassert>
#include <cstring>
#include <stdexcept>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Mesh_GPU::Mesh_GPU(
        const Vulkan_Device& _device,
        VkCommandBuffer            _transfer_cmd,
        const CoreTypes::MeshData& _mesh_data)

        : device_handle(_device.Get_logical_device_handle()),
        vertex_buffer(VK_NULL_HANDLE),
        vertex_buffer_memory(VK_NULL_HANDLE),
        index_buffer(VK_NULL_HANDLE),
        index_buffer_memory(VK_NULL_HANDLE),
        vertex_staging_buffer(VK_NULL_HANDLE),
        vertex_staging_memory(VK_NULL_HANDLE),
        index_staging_buffer(VK_NULL_HANDLE),
        index_staging_memory(VK_NULL_HANDLE),
        vertex_count(static_cast<uint32_t>(_mesh_data.vertices.size())),
        index_count(static_cast<uint32_t>(_mesh_data.indices.size())),
        index_type(_mesh_data.index_type == CoreTypes::Index_Type::UINT16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Mesh_GPU");
        assert(_transfer_cmd != VK_NULL_HANDLE &&
            "Mesh_GPU: transfer command buffer must be valid and already recording");
        assert(!_mesh_data.vertices.empty() &&
            "Mesh_GPU: MeshData has no vertices");
        assert(!_mesh_data.indices.empty() &&
            "Mesh_GPU: MeshData has no indices");

        // =========================================================
        // Vertex buffer
        // =========================================================

        const VkDeviceSize vertex_buffer_size =
            sizeof(CoreTypes::Vertex_Static_Mesh) * _mesh_data.vertices.size();

        // Staging: CPU-visible, source of the transfer.
        Vulkan_Buffer_Utils::Create_buffer(
            _device,
            vertex_buffer_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            vertex_staging_buffer,
            vertex_staging_memory
        );

        // Copy vertex data into the staging buffer.
        void* vertex_data = nullptr;
        vkMapMemory(device_handle, vertex_staging_memory, 0, vertex_buffer_size, 0, &vertex_data);
        std::memcpy(vertex_data, _mesh_data.vertices.data(), static_cast<size_t>(vertex_buffer_size));
        vkUnmapMemory(device_handle, vertex_staging_memory);

        // Final: GPU-local, destination of the transfer.
        Vulkan_Buffer_Utils::Create_buffer(
            _device,
            vertex_buffer_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            vertex_buffer,
            vertex_buffer_memory
        );

        // Record the copy — does NOT submit or wait.
        VkBufferCopy vertex_copy{};
        vertex_copy.srcOffset = 0;
        vertex_copy.dstOffset = 0;
        vertex_copy.size = vertex_buffer_size;
        vkCmdCopyBuffer(_transfer_cmd, vertex_staging_buffer, vertex_buffer, 1, &vertex_copy);

        // =========================================================
        // Index buffer
        // =========================================================

        // Index data is stored as uint32_t on the CPU side regardless
        // of index_type. If index_type is UINT16 we convert to uint16_t
        // here so the GPU buffer uses half the memory.
        const bool use_uint16 =
            (_mesh_data.index_type == CoreTypes::Index_Type::UINT16);

        const VkDeviceSize index_buffer_size = use_uint16
            ? sizeof(uint16_t) * _mesh_data.indices.size()
            : sizeof(uint32_t) * _mesh_data.indices.size();

        // Staging
        Vulkan_Buffer_Utils::Create_buffer(
            _device,
            index_buffer_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            index_staging_buffer,
            index_staging_memory
        );

        void* index_data = nullptr;
        vkMapMemory(device_handle, index_staging_memory, 0, index_buffer_size, 0, &index_data);

        if (use_uint16)
        {
            // Downcast uint32_t → uint16_t into the staging buffer.
            uint16_t* dst = static_cast<uint16_t*>(index_data);
            for (size_t i = 0; i < _mesh_data.indices.size(); ++i)
                dst[i] = static_cast<uint16_t>(_mesh_data.indices[i]);
        }
        else
        {
            std::memcpy(index_data,
                _mesh_data.indices.data(),
                static_cast<size_t>(index_buffer_size));
        }

        vkUnmapMemory(device_handle, index_staging_memory);

        // Final
        Vulkan_Buffer_Utils::Create_buffer(
            _device,
            index_buffer_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            index_buffer,
            index_buffer_memory
        );

        VkBufferCopy index_copy{};
        index_copy.srcOffset = 0;
        index_copy.dstOffset = 0;
        index_copy.size = index_buffer_size;
        vkCmdCopyBuffer(_transfer_cmd, index_staging_buffer, index_buffer, 1, &index_copy);
    }

    // ---------- Destructor ----------
    Mesh_GPU::~Mesh_GPU()
    {
        Destroy();
    }

    // ---------- Destroy ----------
    void Mesh_GPU::Destroy()
    {
        // Release staging buffers first (may already be freed if
        // Release_staging_buffers() was called — VK_NULL_HANDLE is safe).
        Release_staging_buffers();

        if (index_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_handle, index_buffer, nullptr);
            index_buffer = VK_NULL_HANDLE;
        }
        if (index_buffer_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_handle, index_buffer_memory, nullptr);
            index_buffer_memory = VK_NULL_HANDLE;
        }
        if (vertex_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_handle, vertex_buffer, nullptr);
            vertex_buffer = VK_NULL_HANDLE;
        }
        if (vertex_buffer_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_handle, vertex_buffer_memory, nullptr);
            vertex_buffer_memory = VK_NULL_HANDLE;
        }
    }

    // ---------- Release_staging_buffers ----------
    void Mesh_GPU::Release_staging_buffers()
    {
        if (vertex_staging_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_handle, vertex_staging_buffer, nullptr);
            vertex_staging_buffer = VK_NULL_HANDLE;
        }
        if (vertex_staging_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_handle, vertex_staging_memory, nullptr);
            vertex_staging_memory = VK_NULL_HANDLE;
        }
        if (index_staging_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_handle, index_staging_buffer, nullptr);
            index_staging_buffer = VK_NULL_HANDLE;
        }
        if (index_staging_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_handle, index_staging_memory, nullptr);
            index_staging_memory = VK_NULL_HANDLE;
        }
    }

    // ---------- Move constructor ----------
    Mesh_GPU::Mesh_GPU(Mesh_GPU&& _other) noexcept
        : device_handle(_other.device_handle),
        vertex_buffer(_other.vertex_buffer),
        vertex_buffer_memory(_other.vertex_buffer_memory),
        index_buffer(_other.index_buffer),
        index_buffer_memory(_other.index_buffer_memory),
        vertex_staging_buffer(_other.vertex_staging_buffer),
        vertex_staging_memory(_other.vertex_staging_memory),
        index_staging_buffer(_other.index_staging_buffer),
        index_staging_memory(_other.index_staging_memory),
        vertex_count(_other.vertex_count),
        index_count(_other.index_count),
        index_type(_other.index_type)
    {
        _other.vertex_buffer = VK_NULL_HANDLE;
        _other.vertex_buffer_memory = VK_NULL_HANDLE;
        _other.index_buffer = VK_NULL_HANDLE;
        _other.index_buffer_memory = VK_NULL_HANDLE;
        _other.vertex_staging_buffer = VK_NULL_HANDLE;
        _other.vertex_staging_memory = VK_NULL_HANDLE;
        _other.index_staging_buffer = VK_NULL_HANDLE;
        _other.index_staging_memory = VK_NULL_HANDLE;
        _other.vertex_count = 0;
        _other.index_count = 0;
    }

    // ---------- Move assignment ----------
    Mesh_GPU& Mesh_GPU::operator=(Mesh_GPU&& _other) noexcept
    {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            vertex_buffer = _other.vertex_buffer;
            vertex_buffer_memory = _other.vertex_buffer_memory;
            index_buffer = _other.index_buffer;
            index_buffer_memory = _other.index_buffer_memory;
            vertex_staging_buffer = _other.vertex_staging_buffer;
            vertex_staging_memory = _other.vertex_staging_memory;
            index_staging_buffer = _other.index_staging_buffer;
            index_staging_memory = _other.index_staging_memory;
            vertex_count = _other.vertex_count;
            index_count = _other.index_count;
            index_type = _other.index_type;

            _other.vertex_buffer = VK_NULL_HANDLE;
            _other.vertex_buffer_memory = VK_NULL_HANDLE;
            _other.index_buffer = VK_NULL_HANDLE;
            _other.index_buffer_memory = VK_NULL_HANDLE;
            _other.vertex_staging_buffer = VK_NULL_HANDLE;
            _other.vertex_staging_memory = VK_NULL_HANDLE;
            _other.index_staging_buffer = VK_NULL_HANDLE;
            _other.index_staging_memory = VK_NULL_HANDLE;
            _other.vertex_count = 0;
            _other.index_count = 0;
        }
        return *this;
    }

    // ---------- Bind ----------
    void Mesh_GPU::Bind(VkCommandBuffer _command_buffer) const
    {
        assert(vertex_buffer != VK_NULL_HANDLE &&
            "Bind() called on a moved-from or destroyed Mesh_GPU");
        assert(_command_buffer != VK_NULL_HANDLE &&
            "Bind() called with a null command buffer");

        VkBuffer     vertex_buffers[] = { vertex_buffer };
        VkDeviceSize offsets[] = { 0 };

        vkCmdBindVertexBuffers(_command_buffer, 0, 1, vertex_buffers, offsets);
        vkCmdBindIndexBuffer(_command_buffer, index_buffer, 0, index_type);
    }

    // ---------- Draw ----------
    void Mesh_GPU::Draw(VkCommandBuffer _command_buffer) const
    {
        assert(vertex_buffer != VK_NULL_HANDLE &&
            "Draw() called on a moved-from or destroyed Mesh_GPU");
        assert(_command_buffer != VK_NULL_HANDLE &&
            "Draw() called with a null command buffer");

        vkCmdDrawIndexed(_command_buffer, index_count, 1, 0, 0, 0);
    }

    // ---------- Getters ----------
    uint32_t Mesh_GPU::Get_index_count() const
    {
        return index_count;
    }

    uint32_t Mesh_GPU::Get_vertex_count() const
    {
        return vertex_count;
    }

} // namespace Renderer