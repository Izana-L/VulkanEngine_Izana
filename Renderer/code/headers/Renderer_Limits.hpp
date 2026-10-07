#pragma once

#include <cstdint>

// The values the shaders also need (draw buckets, frustum planes, workgroup
// sizes) are macros of this file: one definition for C++ and GLSL.
#include "../../shaders/common/gpu_shared.h"

namespace Renderer_System
{

    // Capacities and constants shared by the Renderer, the GPU layouts and
    // the shaders. Plain values only: this header depends on nothing but
    // <cstdint> and gpu_shared.h (macros), so pure CPU code (Cluster_Grid,
    // tests) can use it without pulling in Vulkan.

    // Capacity of the per-frame light buffer. The shader reads an
    // unsized array, so raising this touches only the C++ side.
    //
    // Directional lights are stored first, then the point and spot lights
    // the cluster pass distributes (Renderer::Write_frame_uniforms). A
    // packet with more lights keeps every directional light and drops the
    // local lights beyond the capacity.
    static constexpr uint32_t MAX_LIGHTS = 1024;

    // Capacity of the per-frame object buffer: one Object_GPU per draw,
    // opaque items first, then transparent ones. Draws beyond it are
    // skipped with a single warning. Growing a buffer that descriptors in
    // flight reference is out of scope; raise the constant instead.
    //
    // Also the capacity of the indirect command buffers: one command per
    // opaque object at most.
    static constexpr uint32_t MAX_OBJECTS = 16384;

    // Buckets the GPU opaque paths can split a frame into. A bucket is the
    // set of opaque objects that share a pipeline and a triangle winding
    // (Draw_List_Builder): each one is drawn by its own
    // vkCmdDrawIndexedIndirectCount, with its own region of the draw command
    // buffer and its own counter (Draw_Count_GPU). A frame that needs more
    // buckets than this draws its opaque objects on the CPU path instead.
    // The bucket index of an object travels in 8 bits of Object_GPU::flags,
    // so it cannot exceed 256.
    //
    // The shaders size Frame_UBO::draw_buckets and Draw_Count::bucket_draw_count
    // with the same macro (GPU_MAX_DRAW_BUCKETS, gpu_shared.h): raising it
    // here without touching the shaders is not possible any more.
    static constexpr uint32_t MAX_DRAW_BUCKETS = GPU_MAX_DRAW_BUCKETS;

    // Capacity of the material table (set 2). Append-only: a slot, once
    // written, never changes. Slot 0 is CoreTypes::Default_Material.
    static constexpr uint32_t MAX_MATERIALS = 1024;

    // Capacity of the mesh table (set 2): one Mesh_Info_GPU per mesh gpu
    // id. Gpu ids are never reused, so a released mesh keeps its slot.
    static constexpr uint32_t MAX_MESHES = 4096;

    // =========================================================
    // Clustered lighting
    // =========================================================
    //
    // The view frustum is split into CLUSTER_TILES_X x CLUSTER_TILES_Y
    // screen tiles and CLUSTER_SLICES depth slices. The tile count is
    // fixed, not the tile size in pixels, so no buffer depends on the
    // resolution and nothing is recreated on resize.
    //
    // Depth slices are exponential between the camera's near plane and
    // CLUSTER_MAX_DISTANCE (thinner close to the camera):
    //     slice k starts at  near * (CLUSTER_MAX_DISTANCE / near)^(k / CLUSTER_SLICES)
    // The projection has an infinite far plane, so the distribution needs
    // a finite end of its own. Fragments farther than the start of the
    // last slice use the last slice, whose volume reaches
    // CLUSTER_LAST_SLICE_FAR_DISTANCE: local lights farther than that do
    // not light anything.
    //
    // Cluster index = tile_x + tile_y * TILES_X + slice * TILES_X * TILES_Y,
    // with tile_y growing downwards like gl_FragCoord.y.
    static constexpr uint32_t CLUSTER_TILES_X = 16;
    static constexpr uint32_t CLUSTER_TILES_Y = 9;
    static constexpr uint32_t CLUSTER_SLICES = 24;
    static constexpr uint32_t CLUSTER_COUNT = CLUSTER_TILES_X * CLUSTER_TILES_Y * CLUSTER_SLICES;

    static constexpr float    CLUSTER_MAX_DISTANCE = 200.0f;
    static constexpr float    CLUSTER_LAST_SLICE_FAR_DISTANCE = 1.0e5f;

    // Near distance the grid is built for when the packet's near plane is
    // not a positive, finite distance (the Renderer reports it and uses this
    // one: Renderer::Write_frame_uniforms).
    static constexpr float    CLUSTER_FALLBACK_NEAR_DISTANCE = 0.1f;

    // Capacity of the compacted light index list, sized for an average of
    // CLUSTER_AVERAGE_LIGHTS lights per cluster (4 bytes per entry, one
    // list per frame in flight). When a frame needs more, the clusters that
    // do not fit keep part of their lights and the loss is counted in
    // Cluster_Counters_GPU::dropped_light_count.
    static constexpr uint32_t CLUSTER_AVERAGE_LIGHTS = 64;
    static constexpr uint32_t CLUSTER_LIGHT_INDEX_CAPACITY = CLUSTER_COUNT * CLUSTER_AVERAGE_LIGHTS;

    // =========================================================
    // Compute push constants
    // =========================================================

    // Size of the push constant range of the compute pipeline layout,
    // shared by every compute pipeline. Each compute shader declares its
    // own block of at most this size at offset 0. The graphics pipeline
    // layout declares no push constant range: graphics shaders read
    // everything per draw from the object, material and mesh tables.
    static constexpr uint32_t COMPUTE_PUSH_CONSTANT_SIZE = 16;

    // =========================================================
    // Compute dispatch
    // =========================================================

    // Local size (local_size_x, and _y for the procedural pass) of each
    // compute shader. They come from the macros the shaders use for their
    // layout(local_size_*), so a dispatch can no longer be computed with a
    // size the shader does not have.
    static constexpr uint32_t CULL_GROUP_SIZE = GPU_CULL_GROUP_SIZE;               // cull_objects.comp
    static constexpr uint32_t CLUSTER_GROUP_SIZE = GPU_CLUSTER_GROUP_SIZE;         // cluster_lights.comp
    static constexpr uint32_t PROCEDURAL_GROUP_SIZE = GPU_PROCEDURAL_GROUP_SIZE;   // Procedural.comp

    // Workgroups needed to cover _invocation_count invocations of groups of
    // _group_size: the count rounded up, the dispatch size of a pass with one
    // invocation per element. The shader discards the invocations past the
    // last element (the groups are whole).
    inline constexpr uint32_t Dispatch_group_count(uint32_t _invocation_count, uint32_t _group_size)
    {
        return _invocation_count / _group_size + ((_invocation_count % _group_size) != 0u ? 1u : 0u);
    }

    static_assert(Dispatch_group_count(0, 64) == 0);
    static_assert(Dispatch_group_count(1, 64) == 1);
    static_assert(Dispatch_group_count(64, 64) == 1);
    static_assert(Dispatch_group_count(65, 64) == 2);
    static_assert(Dispatch_group_count(256, 8) == 32);

    // =========================================================
    // Culling
    // =========================================================

    // Planes of the culling frustum carried by Frame_UBO; the same count as
    // CoreTypes::Frustum (left, right, bottom, top, near; no far plane).
    // The shaders size Frame_UBO::frustum_planes with the same macro
    // (GPU_FRUSTUM_PLANE_COUNT, gpu_shared.h). The equality with
    // CoreTypes::Frustum is asserted where both are visible, next to the copy
    // of the planes into Frame_UBO (Renderer.cpp).
    static constexpr uint32_t FRUSTUM_PLANE_COUNT = GPU_FRUSTUM_PLANE_COUNT;

} // namespace Renderer_System
