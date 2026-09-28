#pragma once

#include <Renderer_Limits.hpp>
#include <Gpu_Layouts.hpp>

#include <Matrix.hpp>

#include <cstdint>

namespace Renderer_System::Cluster_Grid
{

    // CPU half of the clustered lighting: the depth slice mapping shared
    // with mesh.frag, and the view space boxes of the clusters that
    // cluster_lights.comp tests lights against. Grid dimensions and
    // distances are the CLUSTER_* constants of Renderer_Limits.hpp. Pure CPU
    // math: the header depends on no Vulkan type.
    //
    // The boxes depend only on the projection and on the near plane: they
    // are rebuilt when either changes (resize, field of view, aspect
    // override), not every frame.

    // Exponential mapping between view distance and depth slice:
    //     slice(d) = floor(log(d) * scale + bias)
    //     start(k) = near * (max / near)^(k / CLUSTER_SLICES)
    // scale and bias travel in Frame_UBO::cluster_slice_scale / _bias, so
    // the fragment shader and the boxes agree on every boundary.
    struct Slice_Mapping
    {
        float near_distance = 0.0f;   // start of slice 0
        float max_distance = 0.0f;    // end of the exponential range (start of slice CLUSTER_SLICES)
        float scale = 0.0f;
        float bias = 0.0f;
    };

    // Mapping for a camera whose near plane is _near_plane (a view
    // distance). A non-positive or non-finite value falls back to 0.1, and
    // the end of the range is kept at least twice the near distance, so
    // the logarithms are always defined.
    Slice_Mapping Make_slice_mapping(float _near_plane);

    // View distance where slice _slice starts, for _slice in
    // [0, CLUSTER_SLICES]; CLUSTER_SLICES yields max_distance.
    float Slice_start_distance(const Slice_Mapping& _mapping, uint32_t _slice);

    // Writes the CLUSTER_COUNT view space boxes of the grid into
    // _out_aabbs, in cluster index order (tile_x + tile_y * TILES_X +
    // slice * TILES_X * TILES_Y).
    //
    // Each tile is a rectangle of normalized device coordinates, tile_y
    // growing downwards like gl_FragCoord.y. Its four corner rays are
    // recovered from the inverse of _projection, and each box encloses the
    // eight points where those rays cross the view distances that bound
    // the slice. Working from the matrix keeps every convention of the
    // projection (reverse-Z, infinite far plane, flipped Y, aspect
    // override, orthographic) without restating it here. The last slice
    // ends at CLUSTER_LAST_SLICE_FAR_DISTANCE, since every fragment beyond
    // the exponential range falls in it.
    //
    // _projection must be invertible, as every perspective and
    // orthographic projection of the Extractor is.
    void Build_aabbs(const MathLib::Matrix4& _projection, const Slice_Mapping& _mapping, Cluster_AABB_GPU* _out_aabbs);

} // namespace Renderer_System::Cluster_Grid
