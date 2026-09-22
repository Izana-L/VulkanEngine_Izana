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
        VmaAllocator               _allocator,
        VkCommandBuffer            _transfer_cmd,
        const CoreTypes::MeshData& _mesh_data)

        : allocator(_allocator),
        vertex_count(static_cast<uint32_t>(_mesh_data.vertices.size())),
        index_count(static_cast<uint32_t>(_mesh_data.indices.size())),
        index_type(_mesh_data.index_type == CoreTypes::Index_Type::UINT16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32)
    {
        assert(allocator != VK_NULL_HANDLE &&
            "Vulkan_Allocator must be fully constructed before creating a Mesh_GPU");
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
        vertex_staging = Vulkan_Buffer_Utils::Create_buffer(
            allocator,
            vertex_buffer_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu,
            true
        );

        // Copy vertex data into the staging buffer.
        Vulkan_Buffer_Utils::Upload_to_buffer(
            allocator,
            vertex_staging,
            _mesh_data.vertices.data(),
            vertex_buffer_size
        );

        // Final: GPU-local, destination of the transfer.
        vertex_buffer = Vulkan_Buffer_Utils::Create_buffer(
            allocator,
            vertex_buffer_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only
        );

        // Record the copy — does NOT submit or wait.
        VkBufferCopy vertex_copy{};
        vertex_copy.srcOffset = 0;
        vertex_copy.dstOffset = 0;
        vertex_copy.size = vertex_buffer_size;
        vkCmdCopyBuffer(_transfer_cmd, vertex_staging.buffer, vertex_buffer.buffer, 1, &vertex_copy);

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
        index_staging = Vulkan_Buffer_Utils::Create_buffer(
            allocator,
            index_buffer_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu,
            true
        );

        if (use_uint16)
        {
            // Downcast uint32_t → uint16_t into the staging buffer.
            // Source and destination layouts differ, so there is no single
            // memcpy to hand to Upload_to_buffer — hence the manual flush.
            uint16_t* dst = static_cast<uint16_t*>(index_staging.mapped_ptr);
            for (size_t i = 0; i < _mesh_data.indices.size(); ++i)
                dst[i] = static_cast<uint16_t>(_mesh_data.indices[i]);

            VK_CHECK(vmaFlushAllocation(allocator, index_staging.allocation, 0, index_buffer_size),
                "Mesh_GPU: flush index staging buffer");
        }
        else
        {
            Vulkan_Buffer_Utils::Upload_to_buffer(
                allocator,
                index_staging,
                _mesh_data.indices.data(),
                index_buffer_size
            );
        }

        // Final
        index_buffer = Vulkan_Buffer_Utils::Create_buffer(
            allocator,
            index_buffer_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only
        );

        VkBufferCopy index_copy{};
        index_copy.srcOffset = 0;
        index_copy.dstOffset = 0;
        index_copy.size = index_buffer_size;
        vkCmdCopyBuffer(_transfer_cmd, index_staging.buffer, index_buffer.buffer, 1, &index_copy);
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
        // Release_staging_buffers() was called — null handles are safe).
        Release_staging_buffers();

        Vulkan_Buffer_Utils::Destroy_buffer(allocator, index_buffer);
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, vertex_buffer);
    }

    // ---------- Release_staging_buffers ----------
    void Mesh_GPU::Release_staging_buffers()
    {
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, vertex_staging);
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, index_staging);
    }

    // ---------- Move constructor ----------
    Mesh_GPU::Mesh_GPU(Mesh_GPU&& _other) noexcept
        : allocator(_other.allocator),
        vertex_buffer(_other.vertex_buffer),
        index_buffer(_other.index_buffer),
        vertex_staging(_other.vertex_staging),
        index_staging(_other.index_staging),
        vertex_count(_other.vertex_count),
        index_count(_other.index_count),
        index_type(_other.index_type)
    {
        _other.vertex_buffer = {};
        _other.index_buffer = {};
        _other.vertex_staging = {};
        _other.index_staging = {};
        _other.vertex_count = 0;
        _other.index_count = 0;
    }

    // ---------- Move assignment ----------
    Mesh_GPU& Mesh_GPU::operator=(Mesh_GPU&& _other) noexcept
    {
        if (this != &_other) {
            Destroy();

            allocator = _other.allocator;
            vertex_buffer = _other.vertex_buffer;
            index_buffer = _other.index_buffer;
            vertex_staging = _other.vertex_staging;
            index_staging = _other.index_staging;
            vertex_count = _other.vertex_count;
            index_count = _other.index_count;
            index_type = _other.index_type;

            _other.vertex_buffer = {};
            _other.index_buffer = {};
            _other.vertex_staging = {};
            _other.index_staging = {};
            _other.vertex_count = 0;
            _other.index_count = 0;
        }
        return *this;
    }

    // ---------- Bind ----------
    void Mesh_GPU::Bind(VkCommandBuffer _command_buffer) const
    {
        assert(vertex_buffer.buffer != VK_NULL_HANDLE &&
            "Bind() called on a moved-from or destroyed Mesh_GPU");
        assert(_command_buffer != VK_NULL_HANDLE &&
            "Bind() called with a null command buffer");

        VkBuffer     vertex_buffers[] = { vertex_buffer.buffer };
        VkDeviceSize offsets[] = { 0 };

        vkCmdBindVertexBuffers(_command_buffer, 0, 1, vertex_buffers, offsets);
        vkCmdBindIndexBuffer(_command_buffer, index_buffer.buffer, 0, index_type);
    }

    // ---------- Draw ----------
    void Mesh_GPU::Draw(VkCommandBuffer _command_buffer) const
    {
        assert(vertex_buffer.buffer != VK_NULL_HANDLE &&
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
