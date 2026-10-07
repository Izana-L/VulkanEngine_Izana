#ifndef CLUSTER_DATA_GLSL
#define CLUSTER_DATA_GLSL

// Set and binding numbers shared with C++.
#include "gpu_shared.h"

// The fragment-to-cluster arithmetic, shared with the shader self-test.
#include "cluster_math.glsl"

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

// Tile and slice of a fragment, and the index of its cluster, for the grid
// of the frame: the pure functions of cluster_math.glsl with the values of
// Frame_UBO.
uvec3 Cluster_coordinates(vec2 _frag_coord, float _view_depth)
{
    return Cluster_coordinates_in_grid(_frag_coord, _view_depth, frame.cluster_grid.xyz, frame.cluster_params);
}

uint Cluster_index(uvec3 _coordinates)
{
    return Cluster_index_in_grid(_coordinates, frame.cluster_grid.xyz);
}

#endif
