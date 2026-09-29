#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Command_Pool.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Handles.hpp>
#include <Geometry_Pool.hpp>
#include <Mesh_Registry.hpp>
#include <MeshData.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Upload_Context: the machinery of synchronous transfers to the GPU
    // (asset uploads and one-off initialization work).
    //
    // One transfer is a command buffer from a dedicated transient pool,
    // recorded by the caller, submitted to the graphics queue and waited
    // for on the CPU:
    //
    //   VkCommandBuffer cmd = context.Begin();
    //   ... record, staging buffers from Create_staging() ...
    //   context.Submit_and_wait(cmd);
    //   context.End(cmd);
    //
    // Staging buffers created through the context stay alive until End() or
    // Abort(), so their contents outlive the GPU copy. A failure at any
    // point is handled by Abort().
    //
    // The pool is separate from the per-frame pools, so a transfer never
    // interferes with a frame in flight. The wait uses a fence, not
    // vkQueueWaitIdle, which would also stall every frame already
    // submitted to the graphics queue.
    //
    // One transfer at a time. Not copyable or movable.
    class Upload_Context
    {
    public:

        Upload_Context(const Vulkan_Device& _device, VmaAllocator _allocator);
        ~Upload_Context();

        Upload_Context(const Upload_Context&) = delete;
        Upload_Context& operator=(const Upload_Context&) = delete;
        Upload_Context(Upload_Context&&) = delete;
        Upload_Context& operator=(Upload_Context&&) = delete;

        // Allocates a primary command buffer and begins it for a single
        // submission. Throws with nothing left allocated.
        VkCommandBuffer Begin();

        // Creates a host-visible, persistently mapped TRANSFER_SRC buffer of
        // _size bytes, released by End() or Abort(). Writes through
        // mapped_ptr need no flush before the submit.
        Vulkan_Buffer_Utils::Buffer_Allocation Create_staging(VkDeviceSize _size);

        // Ends _command_buffer, submits it to the graphics queue signaling
        // the context fence, and blocks until the fence signals.
        void Submit_and_wait(VkCommandBuffer _command_buffer);

        // Success path, after Submit_and_wait: the GPU has consumed every
        // staging buffer, which are destroyed together with the command
        // buffer.
        void End(VkCommandBuffer _command_buffer);

        // Failure path, at any point after Begin: waits for the device to
        // go idle, so nothing recorded can still be executing, then
        // destroys the staging buffers and the command buffer. The caller
        // undoes its own registrations after this returns. Does not throw.
        void Abort(VkCommandBuffer _command_buffer) noexcept;

        // Records the copies that place _meshes into the Geometry_Pool and
        // the GPU mesh table, and the barrier that makes them visible.
        //
        // _first_id: registry id of _meshes[0]; the rest are consecutive
        // (Mesh_Registry::Add_batch), so their table entries form one
        // contiguous region. The ranges were just allocated, so no frame in
        // flight reads them and no barrier is needed before the copies.
        //
        // One staging buffer holds the whole batch: vertices packed into
        // the pool layout, then indices, then the table entries, each
        // section on a 16-byte boundary, with one copy region per mesh for
        // vertices and indices and a single region for the table. Peak
        // host-visible memory is the sum of the batch.
        void Record_mesh_copies(VkCommandBuffer _command_buffer,
                                const std::vector<const CoreTypes::MeshData*>& _meshes,
                                const Mesh_Registry& _registry,
                                uint32_t _first_id,
                                const Geometry_Pool& _pool,
                                VkBuffer _mesh_table_buffer);

    private:

        const Vulkan_Device&                                 device;
        VmaAllocator                                         allocator;

        Vulkan_Command_Pool                                  command_pool;

        // Shared by every transfer and reset before each submit, instead of
        // created and destroyed per transfer. Not created signaled: its
        // state at creation is irrelevant. Owned by a wrapper, so it is
        // released even when the constructor fails after creating it.
        Unique_Fence                                         fence;

        std::vector<Vulkan_Buffer_Utils::Buffer_Allocation>  staging_buffers;

        void Destroy_staging_buffers() noexcept;
    };

} // namespace Renderer_System
