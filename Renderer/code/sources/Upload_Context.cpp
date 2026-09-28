#include <Upload_Context.hpp>
#include <Vulkan_Utils.hpp>
#include <Vertex_Packing.hpp>
#include <Gpu_Layouts.hpp>

#include <cassert>
#include <cstring>
#include <stdexcept>

namespace Renderer_System
{

    namespace
    {
        VkDeviceSize Align_up(VkDeviceSize _value, VkDeviceSize _alignment)
        {
            return (_value + _alignment - 1) / _alignment * _alignment;
        }
    }

    Upload_Context::Upload_Context(const Vulkan_Device& _device, VmaAllocator _allocator)
        : device(_device),
        allocator(_allocator),
        command_pool(_device, 0, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT)
    {
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        VK_CHECK(vkCreateFence(device.Get_logical_device_handle(), &fence_info, nullptr, &fence),
            "Upload_Context: failed to create the transfer fence");
    }

    Upload_Context::~Upload_Context()
    {
        Destroy_staging_buffers();

        if (fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(device.Get_logical_device_handle(), fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
    }

    void Upload_Context::Destroy_staging_buffers() noexcept
    {
        for (Vulkan_Buffer_Utils::Buffer_Allocation& buffer : staging_buffers)
            Vulkan_Buffer_Utils::Destroy_buffer(allocator, buffer);

        staging_buffers.clear();
    }

    VkCommandBuffer Upload_Context::Begin()
    {
        VkCommandBuffer command_buffer = command_pool.Allocate_primary();

        try
        {
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

            VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info), "Upload_Context: begin transfer command buffer");
        }
        catch (...)
        {
            command_pool.Free(command_buffer);
            throw;
        }

        return command_buffer;
    }

    Vulkan_Buffer_Utils::Buffer_Allocation Upload_Context::Create_staging(VkDeviceSize _size)
    {
        // The slot is reserved before the buffer exists, so registering the
        // buffer cannot fail and leak it.
        staging_buffers.reserve(staging_buffers.size() + 1);

        const Vulkan_Buffer_Utils::Buffer_Allocation buffer = Vulkan_Buffer_Utils::Create_buffer(allocator, _size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);

        staging_buffers.push_back(buffer);

        return buffer;
    }

    void Upload_Context::Submit_and_wait(VkCommandBuffer _command_buffer)
    {
        if (_command_buffer == VK_NULL_HANDLE)
            throw std::invalid_argument("Upload_Context::Submit_and_wait: null command buffer");

        const VkDevice device_handle = device.Get_logical_device_handle();

        VK_CHECK(vkEndCommandBuffer(_command_buffer), "Upload_Context::Submit_and_wait: failed to end command buffer");

        // The fence is shared across transfers: unsignal it before reuse.
        VK_CHECK(vkResetFences(device_handle, 1, &fence), "Upload_Context::Submit_and_wait: reset transfer fence");

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &_command_buffer;

        VK_CHECK(vkQueueSubmit(device.Get_graphics_queue(), 1, &submit_info, fence),
            "Upload_Context::Submit_and_wait: failed to submit");

        VK_CHECK(vkWaitForFences(device_handle, 1, &fence, VK_TRUE, UINT64_MAX),
            "Upload_Context::Submit_and_wait: wait for transfer fence");
    }

    void Upload_Context::End(VkCommandBuffer _command_buffer)
    {
        Destroy_staging_buffers();
        command_pool.Free(_command_buffer);
    }

    void Upload_Context::Abort(VkCommandBuffer _command_buffer) noexcept
    {
        // The submission either never ran or was waited for by
        // Submit_and_wait before it threw; the idle wait also covers a
        // failure between the two.
        vkDeviceWaitIdle(device.Get_logical_device_handle());

        Destroy_staging_buffers();
        command_pool.Free(_command_buffer);
    }

    void Upload_Context::Record_mesh_copies(VkCommandBuffer _command_buffer,
                                            const std::vector<const CoreTypes::MeshData*>& _meshes,
                                            const Mesh_Registry& _registry,
                                            uint32_t _first_id,
                                            const Geometry_Pool& _pool,
                                            VkBuffer _mesh_table_buffer)
    {
        const size_t mesh_count = _meshes.size();

        if (mesh_count == 0)
            return;

        VkDeviceSize batch_vertices = 0;
        VkDeviceSize batch_indices = 0;

        for (const CoreTypes::MeshData* mesh_data : _meshes)
        {
            batch_vertices += mesh_data->vertices.size();
            batch_indices += mesh_data->indices.size();
        }

        // Sections of the staging buffer: vertices | indices | table entries.
        const VkDeviceSize vertex_section_size = batch_vertices * sizeof(Geometry_Pool::Vertex);
        const VkDeviceSize index_section = Align_up(vertex_section_size, 16);
        const VkDeviceSize table_section = Align_up(index_section + batch_indices * sizeof(Geometry_Pool::Index), 16);
        const VkDeviceSize staging_size = table_section + mesh_count * sizeof(Mesh_Info_GPU);

        const Vulkan_Buffer_Utils::Buffer_Allocation staging = Create_staging(staging_size);

        uint8_t* const       staging_bytes = static_cast<uint8_t*>(staging.mapped_ptr);
        Mesh_Info_GPU* const table_entries = reinterpret_cast<Mesh_Info_GPU*>(staging_bytes + table_section);

        std::vector<VkBufferCopy> vertex_copies;
        std::vector<VkBufferCopy> index_copies;

        vertex_copies.reserve(mesh_count);
        index_copies.reserve(mesh_count);

        VkDeviceSize vertex_cursor = 0;
        VkDeviceSize index_cursor = index_section;

        for (size_t i = 0; i < mesh_count; ++i)
        {
            const CoreTypes::MeshData& mesh_data = *_meshes[i];
            const Mesh_GPU&            mesh = _registry.Get(_first_id + static_cast<uint32_t>(i));

            // Vertices, packed into the layout the pool (and mesh.vert)
            // reads.
            Geometry_Pool::Vertex* const packed = reinterpret_cast<Geometry_Pool::Vertex*>(staging_bytes + vertex_cursor);

            for (size_t v = 0; v < mesh_data.vertices.size(); ++v)
                packed[v] = CoreTypes::Vertex_Packing::Pack(mesh_data.vertices[v]);

            const VkDeviceSize vertex_size = mesh_data.vertices.size() * sizeof(Geometry_Pool::Vertex);
            vertex_copies.push_back({ vertex_cursor, Geometry_Pool::Vertex_byte_offset(mesh.geometry), vertex_size });
            vertex_cursor += vertex_size;

            // Indices: MeshData keeps them as uint32_t whatever its
            // index_type says, and the pool has a single index type,
            // VK_INDEX_TYPE_UINT32, so they are copied unchanged. They stay
            // local to the mesh: vertexOffset rebases them.
            const VkDeviceSize index_size = mesh_data.indices.size() * sizeof(Geometry_Pool::Index);
            std::memcpy(staging_bytes + index_cursor, mesh_data.indices.data(), static_cast<size_t>(index_size));

            index_copies.push_back({ index_cursor, Geometry_Pool::Index_byte_offset(mesh.geometry), index_size });
            index_cursor += index_size;

            // Mesh table entry, the GPU copy of the record.
            table_entries[i] = mesh.Make_table_entry();
        }

        // The ids of one batch are consecutive, so their table entries are
        // one contiguous region.
        VkBufferCopy table_copy{};
        table_copy.srcOffset = table_section;
        table_copy.dstOffset = static_cast<VkDeviceSize>(_first_id) * sizeof(Mesh_Info_GPU);
        table_copy.size = static_cast<VkDeviceSize>(mesh_count) * sizeof(Mesh_Info_GPU);

        // Three copies for the whole batch, one region per mesh.
        vkCmdCopyBuffer(_command_buffer, staging.buffer, _pool.Get_vertex_buffer(),
            static_cast<uint32_t>(vertex_copies.size()), vertex_copies.data());
        vkCmdCopyBuffer(_command_buffer, staging.buffer, _pool.Get_index_buffer(),
            static_cast<uint32_t>(index_copies.size()), index_copies.data());
        vkCmdCopyBuffer(_command_buffer, staging.buffer, _mesh_table_buffer, 1, &table_copy);

        // A barrier's second scope reaches every later submission of the
        // queue, so this makes the copies visible to all the frames that
        // will read them: vertex and index fetch of the draws, the vertex
        // shader of the bounds view and the culling pass (mesh table).
        Vulkan_Buffer_Utils::Record_memory_barrier(_command_buffer,
            { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT },
            { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT });
    }

} // namespace Renderer_System
