#include <Light_Clusters.hpp>
#include <Cluster_Grid.hpp>
#include <Descriptor_Sets.hpp>
#include <Renderer_Limits.hpp>

#include <algorithm>
#include <cassert>

namespace Renderer_System
{

    namespace
    {
        // local_size_x of cluster_lights.comp.
        constexpr uint32_t CLUSTER_GROUP_SIZE = 64;

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

        VkDescriptorBufferInfo aabb_info{};
        aabb_info.buffer = aabb_buffer.buffer;
        aabb_info.offset = 0;
        aabb_info.range = AABB_BUFFER_SIZE;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = _per_pass_set;
        write.dstBinding = Binding_Per_Pass::Cluster_AABBs;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &aabb_info;

        vkUpdateDescriptorSets(device_handle, 1, &write, 0, nullptr);
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
        Vulkan_Buffer_Utils::Record_memory_barrier(_command_buffer,
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

    void Light_Clusters::Record_dispatch(VkCommandBuffer _command_buffer, VkPipelineLayout _compute_layout,
                                         uint32_t _first_local_light, uint32_t _light_count) const
    {
        vkCmdBindPipeline(_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get_handle());

        Cluster_Push_Constants push{};
        push.cluster_count = CLUSTER_COUNT;
        push.light_index_capacity = CLUSTER_LIGHT_INDEX_CAPACITY;
        push.first_local_light = _first_local_light;
        push.light_count = _light_count;

        // Stage flags must match the range of the compute pipeline layout
        // exactly (VK_SHADER_STAGE_COMPUTE_BIT).
        vkCmdPushConstants(_command_buffer, _compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Cluster_Push_Constants), &push);

        // One invocation per cluster, rounded up to whole groups.
        vkCmdDispatch(_command_buffer, (CLUSTER_COUNT + CLUSTER_GROUP_SIZE - 1) / CLUSTER_GROUP_SIZE, 1, 1);
    }

} // namespace Renderer_System
