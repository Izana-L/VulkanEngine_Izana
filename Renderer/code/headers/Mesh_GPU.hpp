#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Geometry_Pool.hpp>
#include <MeshData.hpp>
#include <Vector.hpp>

#include <cstdint>

namespace Renderer_System
{

    // Mesh_GPU: the Renderer's record of one uploaded mesh.
    //
    // A lightweight record, not a resource owner: the vertices and indices
    // live in the shared Geometry_Pool, and this only says where (the
    // range) plus the bounding volume the culling needs. Copying it copies
    // a description; freeing the geometry is Geometry_Pool::Free on the
    // range, scheduled by the Renderer once no frame in flight draws it.
    //
    // The same data is mirrored on the GPU in the mesh table
    // (Mesh_Info_GPU, set 2), indexed by the same gpu id, for the passes
    // that build draws without the CPU.
    //
    // Drawing assumes the pool is bound (Geometry_Pool::Bind) in the
    // command buffer:
    //
    //   geometry_pool.Bind(cmd);
    //   mesh.Draw(cmd, object_index);
    struct Mesh_GPU
    {
        Geometry_Range   geometry;

        // Bounding sphere in mesh space: center of the AABB of the
        // positions, radius to the farthest vertex. Transformed by the
        // object's model matrix before any test.
        MathLib::Vector3 bounds_center = { 0.0f, 0.0f, 0.0f };
        float            bounds_radius = 0.0f;

        // Set by Renderer::Release_mesh. A released mesh is never drawn
        // again; its range is returned to the pool when the frames in
        // flight that may still read it have completed.
        bool             released = false;

        // Bounding sphere of the positions of _mesh_data, computed from the
        // full-precision CPU vertices: the positions reach the vertex
        // buffer as 32-bit floats, so the sphere encloses exactly what the
        // shader reconstructs. Not the minimal sphere, but at most sqrt(3)
        // times its radius and computed in one pass.
        //
        // _mesh_data must have at least one vertex.
        static void Compute_bounding_sphere(const CoreTypes::MeshData& _mesh_data, MathLib::Vector3& _out_center, float& _out_radius);

        // Records an indexed draw of the whole mesh, one instance.
        //
        // _first_instance is the value gl_InstanceIndex takes in the vertex
        // shader (Vulkan includes firstInstance in it): the index of the
        // draw's entry in the object buffer. A non-zero value needs no
        // device feature on a direct draw.
        void Draw(VkCommandBuffer _command_buffer, uint32_t _first_instance) const;

        // The same draw as an indirect command, for a buffer consumed by
        // vkCmdDrawIndexedIndirect. A non-zero firstInstance in an
        // indirect command needs drawIndirectFirstInstance, which device
        // selection requires.
        VkDrawIndexedIndirectCommand Make_indirect_command(uint32_t _first_instance) const;
    };

} // namespace Renderer_System
