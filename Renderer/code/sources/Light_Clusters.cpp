#include <Light_Clusters.hpp>
#include <Cluster_Grid.hpp>
#include <Descriptor_Layouts.hpp>
#include <Renderer_Limits.hpp>
#include <Vulkan_Barrier.hpp>

#include <algorithm>
#include <cassert>

namespace Renderer_System
{

    namespace
    {
        // The workgroup size of cluster_lights.comp (CLUSTER_GROUP_SIZE) is
        // in Renderer_Limits.hpp, built from the same macro as the
        // local_size of the shader.

        // vkCmdUpdateBuffer accepts at most 65536 bytes per call.
        constexpr VkDeviceSize UPDATE_BUFFER_MAX_BYTES = 65536;

        constexpr VkDeviceSize AABB_BUFFER_SIZE = sizeof(Cluster_AABB_GPU) * CLUSTER_COUNT;
    }

    Light_Clusters::Light_Clusters(const Vulkan_Device& _device, VmaAllocator _allocator,
                                   VkPipelineCache _pipeline_cache, VkPipelineLayout _compute_layout)
        : device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator),
        pipeline(_device, _pipeline_cache, _compute_layout, "..\\..\\Renderer\\shaders\\compiled\\cluster_lights.comp.spv")
    {
        aabb_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator, AABB_BUFFER_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only);

        aabb_scratch.resize(CLUSTER_COUNT);

        // Written by the first frame that reaches the GPU: nothing reads the
        // buffer before it.
        boxes_valid = false;
    }

    Light_Clusters::~Light_Clusters()
    {
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, aabb_buffer);
    }

    void Light_Clusters::Write_descriptor(VkDescriptorSet _per_pass_set) const
    {
        assert(_per_pass_set != VK_NULL_HANDLE && "Light_Clusters::Write_descriptor: the compute set 1 is not allocated");

        // The whole buffer: the boxes of every cluster.
        Vulkan_Descriptor_Utils::Descriptor_Writer writer(Descriptor_Layouts::Compute_Per_Pass);
        writer.Write_buffer(_per_pass_set, Binding_Per_Pass::Cluster_AABBs, aabb_buffer);
        writer.Update(device_handle);
    }

    bool Light_Clusters::Record_aabb_update(VkCommandBuffer _command_buffer, const MathLib::Matrix4& _projection, float _near_plane)
    {
        if (boxes_valid && _projection == built_projection && _near_plane == built_near)
            return false;

        Cluster_Grid::Build_aabbs(_projection, Cluster_Grid::Make_slice_mapping(_near_plane), aabb_scratch.data());

        // Write-after-read: the cluster pass of earlier frames, possibly
        // still executing, reads the single copy of the boxes. An
        // execution dependency is enough: nothing written before has to
        // become visible to the update.
        Vulkan_Barrier::Record_memory_barrier(_command_buffer,
            { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0 },
            { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT });

        // vkCmdUpdateBuffer copies the data into the command buffer at
        // record time, so the CPU array is free again as soon as this
        // returns.
        const uint8_t* const data = reinterpret_cast<const uint8_t*>(aabb_scratch.data());

        for (VkDeviceSize offset = 0; offset < AABB_BUFFER_SIZE; offset += UPDATE_BUFFER_MAX_BYTES)
        {
            const VkDeviceSize chunk = std::min(UPDATE_BUFFER_MAX_BYTES, AABB_BUFFER_SIZE - offset);
            vkCmdUpdateBuffer(_command_buffer, aabb_buffer.buffer, offset, chunk, data + offset);
        }

        // The state of the boxes is not touched here: the frame may still
        // fail before its submit. Commit does it.
        return true;
    }

    void Light_Clusters::Commit(const MathLib::Matrix4& _projection, float _near_plane)
    {
        built_projection = _projection;
        built_near = _near_plane;
        boxes_valid = true;
    }

    void Light_Clusters::Record_dispatch(VkCommandBuffer _command_buffer) const
    {
        // One invocation per cluster, rounded up to whole groups. The shader
        // takes the cluster count from the grid dimensions of Frame_UBO,
        // which Renderer::Write_frame_uniforms fills from the same CLUSTER_*
        // constants.
        pipeline.Dispatch_groups(_command_buffer, Dispatch_group_count(CLUSTER_COUNT, CLUSTER_GROUP_SIZE));
    }

} // namespace Renderer_System
