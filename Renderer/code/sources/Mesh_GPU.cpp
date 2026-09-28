#include <Mesh_GPU.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace Renderer_System
{

    // ---------- Compute_bounding_sphere ----------
    void Mesh_GPU::Compute_bounding_sphere(const CoreTypes::MeshData& _mesh_data, MathLib::Vector3& _out_center, float& _out_radius)
    {
        assert(!_mesh_data.vertices.empty() && "Compute_bounding_sphere: MeshData has no vertices");

        // Center: middle of the axis-aligned box of the positions.
        MathLib::Vector3 box_min = _mesh_data.vertices.front().position;
        MathLib::Vector3 box_max = box_min;

        for (const CoreTypes::Vertex_Static_Mesh_CPU& vertex : _mesh_data.vertices)
        {
            box_min = glm::min(box_min, vertex.position);
            box_max = glm::max(box_max, vertex.position);
        }

        const MathLib::Vector3 center = (box_min + box_max) * 0.5f;

        // Radius: distance to the farthest vertex, so every vertex is
        // inside, whatever the shape of the box.
        float max_distance_squared = 0.0f;

        for (const CoreTypes::Vertex_Static_Mesh_CPU& vertex : _mesh_data.vertices)
        {
            const MathLib::Vector3 offset = vertex.position - center;
            max_distance_squared = std::max(max_distance_squared, glm::dot(offset, offset));
        }

        _out_center = center;
        _out_radius = std::sqrt(max_distance_squared);
    }

    // ---------- Draw ----------
    void Mesh_GPU::Draw(VkCommandBuffer _command_buffer, uint32_t _first_instance) const
    {
        assert(geometry.Is_valid() && "Mesh_GPU::Draw on a mesh without geometry");
        assert(_command_buffer != VK_NULL_HANDLE && "Mesh_GPU::Draw with a null command buffer");

        vkCmdDrawIndexed(_command_buffer, geometry.index_count, 1, geometry.first_index,
            static_cast<int32_t>(geometry.first_vertex), _first_instance);
    }

    // ---------- Make_table_entry ----------
    Mesh_Info_GPU Mesh_GPU::Make_table_entry() const
    {
        Mesh_Info_GPU entry{};
        entry.bounding_sphere = MathLib::Vector4(bounds_center, bounds_radius);
        entry.first_index = geometry.first_index;
        entry.index_count = geometry.index_count;
        entry.vertex_offset = static_cast<int32_t>(geometry.first_vertex);
        entry.vertex_count = geometry.vertex_count;
        return entry;
    }

    // ---------- Make_indirect_command ----------
    VkDrawIndexedIndirectCommand Mesh_GPU::Make_indirect_command(uint32_t _first_instance) const
    {
        VkDrawIndexedIndirectCommand command{};
        command.indexCount = geometry.index_count;
        command.instanceCount = 1;
        command.firstIndex = geometry.first_index;
        command.vertexOffset = static_cast<int32_t>(geometry.first_vertex);
        command.firstInstance = _first_instance;
        return command;
    }

} // namespace Renderer_System
