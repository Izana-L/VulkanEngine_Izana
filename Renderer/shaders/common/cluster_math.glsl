#ifndef CLUSTER_MATH_GLSL
#define CLUSTER_MATH_GLSL

// The arithmetic of the clustered lighting that maps a fragment to its
// cluster, as pure functions of explicit arguments: no descriptor, no
// access to the frame's uniform block. cluster_data.glsl calls them with the
// values of Frame_UBO, and selftest.comp calls the very same code with test
// values, so the shader and its C++ twin (Renderer_System::Cluster_Grid:
// Find_cluster, Cluster_index) can be compared on the real GLSL.
//
//   _grid   - tiles X, tiles Y, slices (Frame_UBO::cluster_grid.xyz);
//   _params - x, y = render size in pixels, z = slice scale, w = slice bias
//             (Frame_UBO::cluster_params).
//
// Cluster index = tile.x + tile.y * tiles_x + slice * tiles_x * tiles_y,
// the order of Renderer_System::Cluster_Grid::Build_aabbs.

// Tile and slice of a fragment. _frag_coord is gl_FragCoord.xy (origin at
// the top left, pixel centers at .5); _view_depth is the distance along
// the view direction, -z in view space. Fragments beyond the last slice
// boundary go to the last slice, closer than the first one to slice 0.
uvec3 Cluster_coordinates_in_grid(vec2 _frag_coord, float _view_depth, uvec3 _grid, vec4 _params)
{
    uvec2 tile = uvec2(_frag_coord * vec2(_grid.xy) / _params.xy);
    tile = min(tile, _grid.xy - uvec2(1u));

    // log of a non-positive value is undefined; such a depth cannot come
    // from a visible fragment, and the clamp keeps it in slice 0.
    const float slice      = floor(log(max(_view_depth, 1e-4)) * _params.z + _params.w);
    const uint  last_slice = _grid.z - 1u;

    return uvec3(tile, uint(clamp(slice, 0.0, float(last_slice))));
}

uint Cluster_index_in_grid(uvec3 _coordinates, uvec3 _grid)
{
    return _coordinates.x
         + _coordinates.y * _grid.x
         + _coordinates.z * _grid.x * _grid.y;
}

#endif
