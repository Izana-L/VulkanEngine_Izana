#include <Mesh_Upload.hpp>
#include <Vulkan_Barrier.hpp>
#include <Vertex_Packing.hpp>
#include <Gpu_Layouts.hpp>

#include <cstring>
#include <stdexcept>
#include <string>

namespace Renderer_System
{

    namespace
    {
        VkDeviceSize Align_up(VkDeviceSize _value, VkDeviceSize _alignment)
        {
            return (_value + _alignment - 1) / _alignment * _alignment;
        }

        [[noreturn]] void Reject_mesh(size_t _batch_index, const std::string& _problem)
        {
            throw std::invalid_argument("Upload_batch: mesh " + std::to_string(_batch_index) + " of the batch " + _problem);
        }
    }

    void Validate_mesh(const CoreTypes::MeshData* _mesh, size_t _batch_index)
    {
        // The text of a message is only built when its check fails.
        if (_mesh == nullptr)
            Reject_mesh(_batch_index, "is a null MeshData pointer");

        if (_mesh->vertices.empty() || _mesh->indices.empty())
            Reject_mesh(_batch_index, "has no vertices or no indices");

        if (_mesh->vertices.size() > UINT32_MAX || _mesh->indices.size() > UINT32_MAX)
            Reject_mesh(_batch_index, "has more than 2^32 - 1 vertices or indices");

        if (_mesh->indices.size() % 3 != 0)
        {
            Reject_mesh(_batch_index, "has " + std::to_string(_mesh->indices.size()) +
                                      " indices, which are not whole triangles (a multiple of 3)");
        }

        const uint32_t vertex_count = static_cast<uint32_t>(_mesh->vertices.size());

        for (size_t i = 0; i < _mesh->indices.size(); ++i)
        {
            if (_mesh->indices[i] >= vertex_count)
            {
                Reject_mesh(_batch_index, "has index " + std::to_string(_mesh->indices[i]) + " at position " + std::to_string(i) +
                                          ", not below its " + std::to_string(vertex_count) + " vertices");
            }
        }
    }

    void Record_mesh_copies(Upload_Context& _upload,
                            VkCommandBuffer _command_buffer,
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

        const Vulkan_Buffer_Utils::Buffer_Allocation staging = _upload.Create_staging(staging_size);

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
                packed[v] = Pack(mesh_data.vertices[v]);

            const VkDeviceSize vertex_size = mesh_data.vertices.size() * sizeof(Geometry_Pool::Vertex);
            vertex_copies.push_back({ vertex_cursor, Geometry_Pool::Vertex_byte_offset(mesh.geometry), vertex_size });
            vertex_cursor += vertex_size;

            // Indices: MeshData keeps them as uint32_t and the pool has a
            // single index type, VK_INDEX_TYPE_UINT32, so they are copied
            // unchanged. They stay local to the mesh: vertexOffset rebases
            // them.
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
        Vulkan_Barrier::Record_memory_barrier(_command_buffer,
            { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT },
            { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT });
    }

} // namespace Renderer_System
