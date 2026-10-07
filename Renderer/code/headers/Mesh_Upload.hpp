#pragma once

#include <vulkan/vulkan.h>

#include <Upload_Context.hpp>
#include <Geometry_Pool.hpp>
#include <Mesh_Registry.hpp>
#include <MeshData.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

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
