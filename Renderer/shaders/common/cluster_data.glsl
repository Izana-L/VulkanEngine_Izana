#ifndef CLUSTER_DATA_GLSL
#define CLUSTER_DATA_GLSL

// Set and binding numbers shared with C++.
#include "gpu_shared.h"

// Clustered lighting: the per-cluster light lists of the frame and the
// mapping from a fragment to its cluster. Requires frame_set.glsl, included
// before this file.
//
// Set 0 (per frame, one copy per frame in flight):
//   binding 3 (Binding_Per_Frame::Cluster_Grid)          - one uvec2 per
//       cluster: x = first entry of the cluster in the light index list,
//       y = number of entries (bits 0-30) and CLUSTER_OVERFLOW_BIT;
//   binding 4 (Binding_Per_Frame::Cluster_Light_Indices) - the light
//       indices of every cluster, compacted one list after another.
// Both are written by cluster_lights.comp and read by mesh.frag. A shader
// that writes them defines CLUSTER_DATA_WRITABLE before the include; every
// other one gets them readonly.
//
// Cluster index = tile.x + tile.y * tiles_x + slice * tiles_x * tiles_y,
// the order of Renderer_System::Cluster_Grid::Build_aabbs.

#ifdef CLUSTER_DATA_WRITABLE
#define CLUSTER_DATA_ACCESS
#else
#define CLUSTER_DATA_ACCESS readonly
#endif

// Bit 31 of the count: the cluster lost lights because the index list was
// full. Mirror of the comment on Renderer_System::Cluster_Range_GPU.
const uint CLUSTER_OVERFLOW_BIT = 0x80000000u;
const uint CLUSTER_COUNT_MASK   = 0x7FFFFFFFu;

// EXACT mirror of Renderer_System::Cluster_AABB_GPU, std430, 32 bytes:
// view space box of one cluster (w unused).
struct Cluster_AABB
{
    vec4 min_point;
    vec4 max_point;
};

layout(set = GPU_SET_PER_FRAME, binding = GPU_BINDING_CLUSTER_GRID, std430) CLUSTER_DATA_ACCESS buffer Cluster_Grid_Buffer
{
    uvec2 ranges[];
} cluster_grid;

layout(set = GPU_SET_PER_FRAME, binding = GPU_BINDING_CLUSTER_LIGHT_INDICES, std430) CLUSTER_DATA_ACCESS buffer Cluster_Light_Index_Buffer
{
    uint indices[];
} cluster_light_indices;

// Tile and slice of a fragment. _frag_coord is gl_FragCoord.xy (origin at
// the top left, pixel centers at .5); _view_depth is the distance along
// the view direction, -z in view space. Fragments beyond the last slice
// boundary go to the last slice, closer than the first one to slice 0.
uvec3 Cluster_coordinates(vec2 _frag_coord, float _view_depth)
{
    uvec2 tile = uvec2(_frag_coord * vec2(frame.cluster_grid.xy) / frame.cluster_params.xy);
    tile = min(tile, frame.cluster_grid.xy - uvec2(1u));

    // log of a non-positive value is undefined; such a depth cannot come
    // from a visible fragment, and the clamp keeps it in slice 0.
    const float slice = floor(log(max(_view_depth, 1e-4)) * frame.cluster_params.z + frame.cluster_params.w);
    const uint  last_slice = frame.cluster_grid.z - 1u;

    return uvec3(tile, uint(clamp(slice, 0.0, float(last_slice))));
}

uint Cluster_index(uvec3 _coordinates)
{
    return _coordinates.x
         + _coordinates.y * frame.cluster_grid.x
         + _coordinates.z * frame.cluster_grid.x * frame.cluster_grid.y;
}

#endif
