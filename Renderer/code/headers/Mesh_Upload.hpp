#pragma once

#include <vulkan/vulkan.h>

#include <Upload_Context.hpp>
#include <Geometry_Pool.hpp>
#include <Mesh_Registry.hpp>
#include <MeshData.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Throws std::invalid_argument unless _mesh is geometry the pool and the
    // draws can use safely. Run on every mesh of a batch before anything
    // touches the GPU (Renderer::Upload_batch), so a bad element cannot
    // leave half a batch uploaded. _batch_index is the position of the mesh
    // in the batch, for the message. Checks that:
    //   - the pointer is not null, and the mesh has vertices and indices;
    //   - neither count exceeds 2^32 - 1;
    //   - the indices form whole triangles (a multiple of three: the
    //     pipelines draw triangle lists);
    //   - every index is below the vertex count. The indices are local to
    //     the mesh and rebased by vertexOffset, so an index past the end
    //     would read the vertices of whatever mesh sits next in the pool, or
    //     past the pool, with no error from the driver.
    void Validate_mesh(const CoreTypes::MeshData* _mesh, size_t _batch_index);

    // Records the copies that place _meshes into the Geometry_Pool and the
    // GPU mesh table, and the barrier that makes them visible, into
    // _command_buffer (a transfer begun with _upload.Begin()).
    //
    // _first_id: registry id of _meshes[0]; the rest are consecutive
    // (Mesh_Registry::Add_batch), so their table entries form one
    // contiguous region. The ranges were just allocated, so no frame in
    // flight reads them and no barrier is needed before the copies.
    //
    // One staging buffer, taken from _upload, holds the whole batch:
    // vertices packed into the pool layout, then indices, then the table
    // entries, each section on a 16-byte boundary, with one copy region per
    // mesh for vertices and indices and a single region for the table.
    // Peak host-visible memory is the sum of the batch. _upload releases it
    // at End() or Abort(), like the staging buffers of the textures.
    void Record_mesh_copies(Upload_Context& _upload,
                            VkCommandBuffer _command_buffer,
                            const std::vector<const CoreTypes::MeshData*>& _meshes,
                            const Mesh_Registry& _registry,
                            uint32_t _first_id,
                            const Geometry_Pool& _pool,
                            VkBuffer _mesh_table_buffer);

} // namespace Renderer_System
