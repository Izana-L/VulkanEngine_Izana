#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Gpu_Layouts.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Compute_Pipeline.hpp>
#include <Vulkan_Device.hpp>
#include <Matrix.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Light_Clusters: the GPU side of the clustered lighting that does not
    // depend on the frame slot: the view space boxes of the clusters and
    // the compute pass that assigns lights to them.
    //
    // The grid dimensions and distances are the CLUSTER_* constants of
    // Renderer_Limits.hpp; the CPU math (slice mapping, box construction)
    // is Cluster_Grid.
    //
    // The boxes depend only on the projection and on the near plane. They
    // live in one device-local buffer shared by every frame in flight, and
    // are rebuilt on the CPU and rewritten inside the frame's command
    // buffer (vkCmdUpdateBuffer) only when either input changes, after a
    // barrier against the reads of earlier frames. The per-frame outputs
    // (cluster grid, light index list, counters) belong to Frame_Data.
    //
    // Owns the boxes buffer and the cluster_lights.comp pipeline. Not
    // copyable or movable.
    class Light_Clusters
    {
    public:

        // _compute_layout: the compute pipeline layout the pass is built
        // and dispatched against (its push constant range holds
        // Cluster_Push_Constants).
        Light_Clusters(const Vulkan_Device& _device, VmaAllocator _allocator,
                       VkPipelineCache _pipeline_cache, VkPipelineLayout _compute_layout);
        ~Light_Clusters();

        Light_Clusters(const Light_Clusters&) = delete;
        Light_Clusters& operator=(const Light_Clusters&) = delete;
        Light_Clusters(Light_Clusters&&) = delete;
        Light_Clusters& operator=(Light_Clusters&&) = delete;

        // Writes the boxes buffer into _per_pass_set
        // (Binding_Per_Pass::Cluster_AABBs). Once, before any frame is
        // recorded: the only moment the set can be updated without racing a
        // frame in flight.
        void Write_descriptor(VkDescriptorSet _per_pass_set) const;

        // Rebuilds the boxes and records their upload when _projection or
        // _near_plane differ from the ones the boxes were built for, and
        // does nothing otherwise. Recorded outside a render pass. The
        // write becomes visible to the cluster pass through the barrier
        // that follows the counter resets, which covers every transfer
        // write before it.
        void Record_aabb_update(VkCommandBuffer _command_buffer, const MathLib::Matrix4& _projection, float _near_plane);

        // Records the assignment pass: one invocation per cluster tests
        // the local lights [_first_local_light, _light_count) of the frame
        // (the directional lights at the start of the buffer are not
        // clustered). The compute descriptor sets must already be bound
        // with _compute_layout.
        void Record_dispatch(VkCommandBuffer _command_buffer, VkPipelineLayout _compute_layout,
                             uint32_t _first_local_light, uint32_t _light_count) const;

        VkBuffer   Get_aabb_buffer() const { return aabb_buffer.buffer; }
        VkPipeline Get_pipeline() const { return pipeline.Get_handle(); }

    private:

        VkDevice                                 device_handle;
        VmaAllocator                             allocator;

        // CLUSTER_COUNT Cluster_AABB_GPU, device-local: written by
        // vkCmdUpdateBuffer (TRANSFER_DST), read by cluster_lights.comp
        // (STORAGE_BUFFER).
        Vulkan_Buffer_Utils::Buffer_Allocation   aabb_buffer;

        // CPU build of the boxes (Cluster_Grid::Build_aabbs), reused.
        std::vector<Cluster_AABB_GPU>            aabb_scratch;

        // Projection and near plane the boxes were built for.
        MathLib::Matrix4                         built_projection{ 0.0f };
        float                                    built_near = 0.0f;
        bool                                     boxes_valid = false;

        Vulkan_Compute_Pipeline                  pipeline;
    };

} // namespace Renderer_System
