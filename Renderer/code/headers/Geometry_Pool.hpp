#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Buffer_Utils.hpp>
#include <Vertex.hpp>

#include <vk_mem_alloc.h>

#include <cstdint>

namespace Renderer_System
{

    // Geometry_Range: where one mesh lives inside the Geometry_Pool, in the
    // units the draw commands take:
    //   first_vertex - vertexOffset of the draw, counted in VERTICES;
    //   first_index  - firstIndex of the draw, counted in INDICES.
    // Neither is a byte offset: the pool sub-allocates in elements, so the
    // values go into vkCmdDrawIndexed / VkDrawIndexedIndirectCommand as
    // they are. The indices of a mesh are local to it (0 is its first
    // vertex); vertexOffset rebases them at draw time.
    //
    // The two VMA virtual allocations identify the range for
    // Geometry_Pool::Free; nothing else reads them.
    struct Geometry_Range
    {
        uint32_t first_vertex = 0;
        uint32_t vertex_count = 0;
        uint32_t first_index = 0;
        uint32_t index_count = 0;

        VmaVirtualAllocation vertex_allocation = VK_NULL_HANDLE;
        VmaVirtualAllocation index_allocation = VK_NULL_HANDLE;

        bool Is_valid() const
        {
            return vertex_allocation != VK_NULL_HANDLE && index_allocation != VK_NULL_HANDLE;
        }
    };

    // Geometry_Allocator: where geometry ranges come from and go back to.
    // Geometry_Pool is the implementation that owns real buffers; consumers
    // that only do accounting (Mesh_Registry) depend on this interface, so
    // they can run against a stand-in without a device.
    class Geometry_Allocator
    {
    public:

        virtual ~Geometry_Allocator() = default;

        // Reserves _vertex_count vertices and _index_count indices, both
        // non-zero. Strong guarantee: throws std::runtime_error, with the
        // allocator unchanged, when the request does not fit.
        virtual Geometry_Range Allocate(uint32_t _vertex_count, uint32_t _index_count) = 0;

        // Returns _range and resets it. Safe on an invalid (default or
        // already freed) range.
        virtual void Free(Geometry_Range& _range) = 0;
    };

    // Geometry_Pool: the vertex and index buffers every static mesh shares,
    // so a frame binds geometry once and any number of draws, direct or
    // indirect, address their mesh through firstIndex / vertexOffset.
    //
    //   vertex buffer - Vertex_Static_Mesh (the layout mesh.vert reads),
    //                   usage VERTEX_BUFFER | TRANSFER_DST | STORAGE_BUFFER;
    //   index buffer  - uint32_t, usage INDEX_BUFFER | TRANSFER_DST |
    //                   STORAGE_BUFFER.
    // STORAGE_BUFFER is not used today; it keeps vertex pulling possible
    // without recreating the buffers.
    //
    // One index type for every mesh (VK_INDEX_TYPE_UINT32): a bound index
    // buffer has a single index type, so every mesh carries uint32_t
    // indices (a glTF with 16-bit indices is widened when it is loaded).
    //
    // One vertex format per pool: meshes with another vertex layout (a
    // skinned mesh, a debug vertex) need a pool of their own.
    //
    // Sub-allocation goes through two VMA virtual blocks, one per buffer,
    // measured in elements (vertices, indices). The capacity is fixed:
    // Allocate throws when a block has no contiguous free range large
    // enough, instead of growing the buffers under descriptors and
    // commands in flight.
    //
    // The pool only allocates and binds. Uploading is the caller's: it
    // copies into Get_vertex_buffer() / Get_index_buffer() at
    // Vertex_byte_offset / Index_byte_offset of the range, and makes the
    // copy visible to the vertex input stage with a barrier.
    //
    // Freeing is also the caller's responsibility to schedule: a range may
    // only be freed once no pending command buffer draws from it
    // (Mesh_Registry defers it until the frames that may read it have
    // completed).
    //
    // Not copyable or movable: owns the two buffers and the two blocks.
    class Geometry_Pool final : public Geometry_Allocator
    {
    public:

        using Vertex = CoreTypes::Vertex_Static_Mesh;
        using Index = uint32_t;

        static constexpr VkIndexType INDEX_TYPE = VK_INDEX_TYPE_UINT32;

        // Creates both buffers (device-local) and both virtual blocks.
        // Capacities are in elements. Throws std::invalid_argument if a
        // capacity is zero.
        Geometry_Pool(VmaAllocator _allocator, uint32_t _vertex_capacity, uint32_t _index_capacity);
        ~Geometry_Pool() override;

        Geometry_Pool(const Geometry_Pool&) = delete;
        Geometry_Pool& operator=(const Geometry_Pool&) = delete;
        Geometry_Pool(Geometry_Pool&&) = delete;
        Geometry_Pool& operator=(Geometry_Pool&&) = delete;

        // Reserves _vertex_count vertices and _index_count indices, both
        // non-zero. Strong guarantee: throws std::runtime_error, with the
        // pool unchanged, when either block has no contiguous free range
        // large enough.
        Geometry_Range Allocate(uint32_t _vertex_count, uint32_t _index_count) override;

        // Returns _range to the pool and resets it. Safe on an invalid
        // (default or already freed) range.
        void Free(Geometry_Range& _range) override;

        // Binds the vertex buffer (binding 0) and the index buffer
        // (VK_INDEX_TYPE_UINT32). Once per command buffer: every draw from
        // the pool shares both bindings.
        void Bind(VkCommandBuffer _command_buffer) const;

        VkBuffer Get_vertex_buffer() const { return vertex_buffer.buffer; }
        VkBuffer Get_index_buffer() const { return index_buffer.buffer; }

        // Byte offsets of a range inside the buffers, for the upload copies.
        static VkDeviceSize Vertex_byte_offset(const Geometry_Range& _range)
        {
            return static_cast<VkDeviceSize>(_range.first_vertex) * sizeof(Vertex);
        }

        static VkDeviceSize Index_byte_offset(const Geometry_Range& _range)
        {
            return static_cast<VkDeviceSize>(_range.first_index) * sizeof(Index);
        }

        uint32_t Get_vertex_capacity() const { return vertex_capacity; }
        uint32_t Get_index_capacity() const { return index_capacity; }

        // Elements currently allocated, for statistics.
        uint32_t Get_used_vertices() const { return used_vertices; }
        uint32_t Get_used_indices() const { return used_indices; }

    private:

        void Destroy();

        VmaAllocator allocator;

        Vulkan_Buffer_Utils::Buffer_Allocation vertex_buffer;
        Vulkan_Buffer_Utils::Buffer_Allocation index_buffer;

        VmaVirtualBlock vertex_block = VK_NULL_HANDLE;
        VmaVirtualBlock index_block = VK_NULL_HANDLE;

        uint32_t vertex_capacity;
        uint32_t index_capacity;
        uint32_t used_vertices = 0;
        uint32_t used_indices = 0;
    };

} // namespace Renderer_System
