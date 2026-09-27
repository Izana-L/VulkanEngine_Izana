#include <Geometry_Pool.hpp>
#include <Vulkan_Utils.hpp>

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

namespace Renderer_System
{

    Geometry_Pool::Geometry_Pool(VmaAllocator _allocator, uint32_t _vertex_capacity, uint32_t _index_capacity)
        : allocator(_allocator),
        vertex_capacity(_vertex_capacity),
        index_capacity(_index_capacity)
    {
        assert(allocator != VK_NULL_HANDLE && "Vulkan_Allocator must be fully constructed before the Geometry_Pool");

        if (_vertex_capacity == 0 || _index_capacity == 0)
            throw std::invalid_argument("Geometry_Pool: capacities must be greater than zero");

        // The destructor does not run for a constructor that throws, so
        // whatever was created before the failure is released here.
        try
        {
            vertex_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator,
                static_cast<VkDeviceSize>(vertex_capacity) * sizeof(Vertex),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only);

            index_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator,
                static_cast<VkDeviceSize>(index_capacity) * sizeof(Index),
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only);

            // Virtual blocks measured in elements: an allocation of N
            // vertices returns the index of its first vertex, which is
            // directly the vertexOffset of the draw.
            VmaVirtualBlockCreateInfo vertex_block_info{};
            vertex_block_info.size = vertex_capacity;

            VK_CHECK(vmaCreateVirtualBlock(&vertex_block_info, &vertex_block),
                "Geometry_Pool: failed to create the vertex virtual block");

            VmaVirtualBlockCreateInfo index_block_info{};
            index_block_info.size = index_capacity;

            VK_CHECK(vmaCreateVirtualBlock(&index_block_info, &index_block),
                "Geometry_Pool: failed to create the index virtual block");
        }
        catch (...)
        {
            Destroy();
            throw;
        }

        std::cout << "[Geometry_Pool] Created: " << vertex_capacity << " vertices ("
            << (static_cast<VkDeviceSize>(vertex_capacity) * sizeof(Vertex)) / (1024 * 1024) << " MB), "
            << index_capacity << " indices ("
            << (static_cast<VkDeviceSize>(index_capacity) * sizeof(Index)) / (1024 * 1024) << " MB).\n";
    }

    Geometry_Pool::~Geometry_Pool()
    {
        Destroy();
    }

    void Geometry_Pool::Destroy()
    {
        // Ranges still allocated (meshes never released) belong to a
        // Renderer that is being destroyed: clearing the blocks releases
        // them all, and vmaDestroyVirtualBlock asserts on a non-empty block.
        if (vertex_block != VK_NULL_HANDLE)
        {
            vmaClearVirtualBlock(vertex_block);
            vmaDestroyVirtualBlock(vertex_block);
            vertex_block = VK_NULL_HANDLE;
        }

        if (index_block != VK_NULL_HANDLE)
        {
            vmaClearVirtualBlock(index_block);
            vmaDestroyVirtualBlock(index_block);
            index_block = VK_NULL_HANDLE;
        }

        Vulkan_Buffer_Utils::Destroy_buffer(allocator, index_buffer);
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, vertex_buffer);

        used_vertices = 0;
        used_indices = 0;
    }

    Geometry_Range Geometry_Pool::Allocate(uint32_t _vertex_count, uint32_t _index_count)
    {
        assert(vertex_block != VK_NULL_HANDLE && index_block != VK_NULL_HANDLE);

        if (_vertex_count == 0 || _index_count == 0)
            throw std::invalid_argument("Geometry_Pool::Allocate: a mesh needs at least one vertex and one index");

        Geometry_Range range;

        // Alignment 1 element: vertices and indices are addressed
        // individually, so no padding between meshes is needed.
        VmaVirtualAllocationCreateInfo vertex_info{};
        vertex_info.size = _vertex_count;
        vertex_info.alignment = 1;

        VkDeviceSize vertex_offset = 0;

        if (vmaVirtualAllocate(vertex_block, &vertex_info, &range.vertex_allocation, &vertex_offset) != VK_SUCCESS)
        {
            throw std::runtime_error("Geometry_Pool: no free range of " + std::to_string(_vertex_count) +
                " vertices (" + std::to_string(used_vertices) + " of " + std::to_string(vertex_capacity) +
                " in use); raise the vertex capacity of the pool");
        }

        VmaVirtualAllocationCreateInfo index_info{};
        index_info.size = _index_count;
        index_info.alignment = 1;

        VkDeviceSize index_offset = 0;

        if (vmaVirtualAllocate(index_block, &index_info, &range.index_allocation, &index_offset) != VK_SUCCESS)
        {
            // Strong guarantee: the vertex half is returned before throwing.
            vmaVirtualFree(vertex_block, range.vertex_allocation);

            throw std::runtime_error("Geometry_Pool: no free range of " + std::to_string(_index_count) +
                " indices (" + std::to_string(used_indices) + " of " + std::to_string(index_capacity) +
                " in use); raise the index capacity of the pool");
        }

        range.first_vertex = static_cast<uint32_t>(vertex_offset);
        range.vertex_count = _vertex_count;
        range.first_index = static_cast<uint32_t>(index_offset);
        range.index_count = _index_count;

        used_vertices += _vertex_count;
        used_indices += _index_count;

        return range;
    }

    void Geometry_Pool::Free(Geometry_Range& _range)
    {
        if (_range.vertex_allocation != VK_NULL_HANDLE)
        {
            vmaVirtualFree(vertex_block, _range.vertex_allocation);
            used_vertices -= _range.vertex_count;
        }

        if (_range.index_allocation != VK_NULL_HANDLE)
        {
            vmaVirtualFree(index_block, _range.index_allocation);
            used_indices -= _range.index_count;
        }

        _range = Geometry_Range{};
    }

    void Geometry_Pool::Bind(VkCommandBuffer _command_buffer) const
    {
        assert(_command_buffer != VK_NULL_HANDLE && "Geometry_Pool::Bind: null command buffer");

        const VkBuffer     vertex_buffers[] = { vertex_buffer.buffer };
        const VkDeviceSize offsets[] = { 0 };

        vkCmdBindVertexBuffers(_command_buffer, 0, 1, vertex_buffers, offsets);
        vkCmdBindIndexBuffer(_command_buffer, index_buffer.buffer, 0, INDEX_TYPE);
    }

} // namespace Renderer_System
