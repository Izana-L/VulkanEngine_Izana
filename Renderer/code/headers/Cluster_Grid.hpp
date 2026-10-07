#pragma once

#include <Renderer_Limits.hpp>
#include <Gpu_Layouts.hpp>

#include <Matrix.hpp>

#include <cstdint>
#include <string>

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

    // True when _near_plane can start the slices: a positive, finite view
    // distance.
    bool Is_valid_near_plane(float _near_plane);

    // Mapping for a camera whose near plane is _near_plane (a view
    // distance). The end of the range is kept at least twice the near
    // distance, so the logarithms are always defined.
    //
    // Throws std::invalid_argument unless Is_valid_near_plane(_near_plane):
    // a pure function does not invent a distance. The Renderer decides what
    // to do with a packet whose near plane is not one (it reports it and
    // builds the grid for CLUSTER_FALLBACK_NEAR_DISTANCE).
    Slice_Mapping Make_slice_mapping(float _near_plane);

    // View distance where slice _slice starts, for _slice in
    // [0, CLUSTER_SLICES]; CLUSTER_SLICES yields max_distance.
    float Slice_start_distance(const Slice_Mapping& _mapping, uint32_t _slice);

    // =========================================================
    // Fragment to cluster: the C++ twin of cluster_math.glsl
    // =========================================================
    //
    // The fragment shader finds the cluster of a fragment with
    // Cluster_coordinates_in_grid and Cluster_index_in_grid
    // (cluster_math.glsl); the boxes above are built with the inverse
    // mapping. The formulas below are the CPU twin of those two functions,
    // written in the same operations and the same order, and the only C++
    // implementation of them: Build_aabbs takes the index from here. Two
    // checks keep the three copies (the shader, this twin, the boxes) from
    // drifting apart: Verify_consistency, which pins the twin against the
    // boxes, and the startup test that runs the real GLSL on the GPU and
    // compares it with this twin (Shader_Self_Test).

    // Tile and depth slice of a fragment. tile_y grows downwards, like
    // gl_FragCoord.y.
    struct Cluster_Coordinates
    {
        uint32_t tile_x = 0;
        uint32_t tile_y = 0;
        uint32_t slice = 0;
    };

    // Cluster of the fragment at (_frag_x, _frag_y) pixels, at distance
    // _view_depth along the view direction, in a frame of _render_width x
    // _render_height pixels, with the slice mapping _mapping (the values
    // that travel in Frame_UBO::cluster_params). Fragments beyond the last
    // slice boundary go to the last slice, closer than the first one to
    // slice 0, and tiles are clamped to the grid.
    Cluster_Coordinates Find_cluster(const Slice_Mapping& _mapping, float _render_width, float _render_height,
                                     float _frag_x, float _frag_y, float _view_depth);

    // tile_x + tile_y * CLUSTER_TILES_X + slice * CLUSTER_TILES_X * CLUSTER_TILES_Y:
    // the index of the cluster in every buffer of the grid, the order of
    // Build_aabbs.
    uint32_t Cluster_index(const Cluster_Coordinates& _coordinates);

    // Checks the pieces of the cluster mapping against each other and
    // returns a description of the first disagreement, or an empty string
    // when they all agree:
    //   - the depth slice of a distance just beyond the start of slice k is
    //     k, and just before it k - 1, for every slice and for several near
    //     planes (Find_cluster against Slice_start_distance);
    //   - a point at the center of a tile and the middle of a slice, taken
    //     through the forward projection (perspective and orthographic,
    //     reverse-Z, Y flipped like the Extractor's), is mapped by
    //     Find_cluster to that tile and slice and lies inside the box
    //     Build_aabbs wrote for that cluster index.
    // Pure CPU, a few milliseconds: run once at startup in every build.
    std::string Verify_consistency();

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
