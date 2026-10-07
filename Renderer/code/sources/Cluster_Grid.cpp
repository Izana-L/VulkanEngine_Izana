#include <Cluster_Grid.hpp>

#include <Vector.hpp>
#include <Vector3.hpp>
#include <Matrix4.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace Renderer_System::Cluster_Grid
{

    namespace
    {
        // A tile corner ray in view space, as two points on it.
        struct View_Ray
        {
            MathLib::Vector3 a;
            MathLib::Vector3 b;
        };

        // Point of the ray whose view space z is -_distance (the camera
        // looks down -Z). Falls back to the first point pushed to that
        // depth when the ray runs parallel to the image plane, which no
        // tile corner of a valid projection does.
        MathLib::Vector3 Point_at_distance(const View_Ray& _ray, float _distance)
        {
            const float target_z = -_distance;
            const float delta_z = _ray.b.z - _ray.a.z;

            if (std::abs(delta_z) < 1e-12f)
                return  MathLib::Vector3(_ray.a.x, _ray.a.y, target_z);

            const float t = (target_z - _ray.a.z) / delta_z;
            return _ray.a + (_ray.b - _ray.a) * t;
        }
    }

    bool Is_valid_near_plane(float _near_plane)
    {
        return std::isfinite(_near_plane) && _near_plane > 0.0f;
    }

    Slice_Mapping Make_slice_mapping(float _near_plane)
    {
        if (!Is_valid_near_plane(_near_plane))
        {
            throw std::invalid_argument("Cluster_Grid::Make_slice_mapping: the near plane (" + std::to_string(_near_plane) +
                                        ") is not a positive, finite distance");
        }

        Slice_Mapping mapping;

        mapping.near_distance = _near_plane;
        mapping.max_distance = std::max(CLUSTER_MAX_DISTANCE, mapping.near_distance * 2.0f);

        const float log_ratio = std::log(mapping.max_distance / mapping.near_distance);
        const float slices = static_cast<float>(CLUSTER_SLICES);

        mapping.scale = slices / log_ratio;
        mapping.bias = -slices * std::log(mapping.near_distance) / log_ratio;

        return mapping;
    }

    Cluster_Coordinates Find_cluster(const Slice_Mapping& _mapping, float _render_width, float _render_height,
                                     float _frag_x, float _frag_y, float _view_depth)
    {
        // Twin of Cluster_coordinates_in_grid (cluster_math.glsl): the same
        // operations in the same order. The pixel to tile conversion
        // truncates like uvec2(vec2); a negative coordinate (undefined in
        // GLSL, and never a fragment) is taken as 0.
        const auto tile_of = [](float _coordinate, uint32_t _tiles, float _render_size)
            {
                const float scaled = _coordinate * static_cast<float>(_tiles) / _render_size;
                const uint32_t tile = scaled > 0.0f ? static_cast<uint32_t>(scaled) : 0u;
                return std::min(tile, _tiles - 1u);
            };

        const float slice = std::floor(std::log(std::max(_view_depth, 1e-4f)) * _mapping.scale + _mapping.bias);
        const float last_slice = static_cast<float>(CLUSTER_SLICES - 1u);

        Cluster_Coordinates coordinates;
        coordinates.tile_x = tile_of(_frag_x, CLUSTER_TILES_X, _render_width);
        coordinates.tile_y = tile_of(_frag_y, CLUSTER_TILES_Y, _render_height);
        coordinates.slice = static_cast<uint32_t>(std::clamp(slice, 0.0f, last_slice));

        return coordinates;
    }

    uint32_t Cluster_index(const Cluster_Coordinates& _coordinates)
    {
        // Twin of Cluster_index_in_grid (cluster_math.glsl).
        return _coordinates.tile_x
             + _coordinates.tile_y * CLUSTER_TILES_X
             + _coordinates.slice * CLUSTER_TILES_X * CLUSTER_TILES_Y;
    }

    float Slice_start_distance(const Slice_Mapping& _mapping, uint32_t _slice)
    {
        assert(_slice <= CLUSTER_SLICES && "Slice_start_distance: slice out of range");

        const float exponent = static_cast<float>(_slice) / static_cast<float>(CLUSTER_SLICES);
        return _mapping.near_distance * std::pow(_mapping.max_distance / _mapping.near_distance, exponent);
    }

    void Build_aabbs(const MathLib::Matrix4& _projection, const Slice_Mapping& _mapping, Cluster_AABB_GPU* _out_aabbs)
    {
        assert(_out_aabbs != nullptr && "Build_aabbs: null output");

        const MathLib::Matrix4 inverse_projection = MathLib::Mat4::Inverse(_projection);

        // NDC point to view space. Two depths strictly inside the depth
        // range give two points of the ray through (x, y): with reverse-Z,
        // 1.0 is the near plane and 0.5 lies beyond it, for the infinite
        // perspective (distance 2 * near) as for the orthographic
        // projection (halfway to the far plane). Depth 0 is avoided: it is
        // the point at infinity of the perspective projection.
        const auto unproject = [&inverse_projection](float _x, float _y, float _depth)
            {
                const MathLib::Vector4 point = inverse_projection * MathLib::Vector4(_x, _y, _depth, 1.0f);
                return MathLib::Vector3(point) / point.w;
            };

        // Slice boundaries, shared by every tile. The last slice reaches
        // the far distance every fragment beyond the range is clamped into.
        std::array<float, CLUSTER_SLICES + 1> boundaries{};

        for (uint32_t k = 0; k < CLUSTER_SLICES; ++k)
            boundaries[k] = Slice_start_distance(_mapping, k);

        boundaries[CLUSTER_SLICES] = std::max(CLUSTER_LAST_SLICE_FAR_DISTANCE, _mapping.max_distance);

        const float tile_width = 2.0f / static_cast<float>(CLUSTER_TILES_X);
        const float tile_height = 2.0f / static_cast<float>(CLUSTER_TILES_Y);

        for (uint32_t tile_y = 0; tile_y < CLUSTER_TILES_Y; ++tile_y)
        {
            for (uint32_t tile_x = 0; tile_x < CLUSTER_TILES_X; ++tile_x)
            {
                // Framebuffer y grows downwards and maps to NDC y in the
                // same direction ([-1, 1] from top to bottom with a
                // positive viewport height), so tile rows map to NDC rows
                // without a flip; the flip of the projection is inside the
                // matrix.
                const float x0 = -1.0f + tile_width * static_cast<float>(tile_x);
                const float x1 = x0 + tile_width;
                const float y0 = -1.0f + tile_height * static_cast<float>(tile_y);
                const float y1 = y0 + tile_height;

                const std::array<MathLib::Vector2, 4> corners = { MathLib::Vector2(x0, y0), MathLib::Vector2(x1, y0),
                                                                  MathLib::Vector2(x0, y1), MathLib::Vector2(x1, y1) };

                std::array<View_Ray, 4> rays{};

                for (size_t c = 0; c < corners.size(); ++c)
                    rays[c] = { unproject(corners[c].x, corners[c].y, 1.0f), unproject(corners[c].x, corners[c].y, 0.5f) };

                for (uint32_t slice = 0; slice < CLUSTER_SLICES; ++slice)
                {
                    MathLib::Vector3 box_min(std::numeric_limits<float>::max());
                    MathLib::Vector3 box_max(std::numeric_limits<float>::lowest());

                    for (const View_Ray& ray : rays)
                    {
                        for (const float distance : { boundaries[slice], boundaries[slice + 1] })
                        {
                            const MathLib::Vector3 point = Point_at_distance(ray, distance);
                            box_min = MathLib::Vec3::Min(box_min, point);
                            box_max = MathLib::Vec3::Max(box_max, point);
                        }
                    }

                    const uint32_t index = Cluster_index({ tile_x, tile_y, slice });

                    _out_aabbs[index].min_point = MathLib::Vector4(box_min, 0.0f);
                    _out_aabbs[index].max_point = MathLib::Vector4(box_max, 0.0f);
                }
            }
        }
    }

    // =========================================================
    // Verify_consistency
    // =========================================================

    namespace
    {
        // Frame the mapping is checked for: not square, so a swapped axis
        // is noticed.
        constexpr float CHECK_RENDER_WIDTH = 1280.0f;
        constexpr float CHECK_RENDER_HEIGHT = 720.0f;

        // Relative distance from a slice boundary at which the slice of a
        // distance is checked: far beyond the error of a float logarithm
        // (about 1e-7 relative), well inside a slice.
        constexpr float BOUNDARY_MARGIN = 1.0e-3f;

        std::string Describe(const Cluster_Coordinates& _c)
        {
            return "(" + std::to_string(_c.tile_x) + ", " + std::to_string(_c.tile_y) + ", " + std::to_string(_c.slice) + ")";
        }

        // The slice of a distance beside every slice boundary.
        std::string Verify_slice_boundaries(float _near_plane)
        {
            const Slice_Mapping mapping = Make_slice_mapping(_near_plane);

            const auto slice_of = [&](float _depth)
                {
                    return Find_cluster(mapping, CHECK_RENDER_WIDTH, CHECK_RENDER_HEIGHT, 0.5f, 0.5f, _depth).slice;
                };

            for (uint32_t k = 0; k < CLUSTER_SLICES; ++k)
            {
                const float start = Slice_start_distance(mapping, k);

                if (slice_of(start * (1.0f + BOUNDARY_MARGIN)) != k)
                    return "near " + std::to_string(_near_plane) + ": a distance just beyond the start of slice " + std::to_string(k)
                        + " falls in slice " + std::to_string(slice_of(start * (1.0f + BOUNDARY_MARGIN)));

                if (k > 0 && slice_of(start * (1.0f - BOUNDARY_MARGIN)) != k - 1)
                    return "near " + std::to_string(_near_plane) + ": a distance just before the start of slice " + std::to_string(k)
                        + " falls in slice " + std::to_string(slice_of(start * (1.0f - BOUNDARY_MARGIN)));
            }

            if (slice_of(mapping.near_distance * 0.5f) != 0)
                return "near " + std::to_string(_near_plane) + ": a distance closer than the near plane is not in slice 0";

            if (slice_of(mapping.max_distance * 10.0f) != CLUSTER_SLICES - 1 || slice_of(CLUSTER_LAST_SLICE_FAR_DISTANCE) != CLUSTER_SLICES - 1)
                return "near " + std::to_string(_near_plane) + ": a distance beyond the exponential range is not in the last slice";

            return {};
        }

        // The center of every tile at the middle of every slice, taken
        // through the forward projection _projection: view space point of
        // normalized device coordinates (_ndc_x, _ndc_y) at view distance
        // _depth, from _point_at.
        template <typename Point_At>
        std::string Verify_boxes(const char* _name, const MathLib::Matrix4& _projection, float _near_plane, Point_At _point_at)
        {
            const Slice_Mapping mapping = Make_slice_mapping(_near_plane);

            std::vector<Cluster_AABB_GPU> boxes(CLUSTER_COUNT);
            Build_aabbs(_projection, mapping, boxes.data());

            for (uint32_t slice = 0; slice < CLUSTER_SLICES; ++slice)
            {
                // Geometric middle of the slice: the middle of its
                // exponential range, whatever the distance it starts at.
                const float depth = std::sqrt(Slice_start_distance(mapping, slice) * Slice_start_distance(mapping, slice + 1));

                for (uint32_t tile_y = 0; tile_y < CLUSTER_TILES_Y; ++tile_y)
                {
                    for (uint32_t tile_x = 0; tile_x < CLUSTER_TILES_X; ++tile_x)
                    {
                        const float ndc_x = -1.0f + 2.0f * (static_cast<float>(tile_x) + 0.5f) / static_cast<float>(CLUSTER_TILES_X);
                        const float ndc_y = -1.0f + 2.0f * (static_cast<float>(tile_y) + 0.5f) / static_cast<float>(CLUSTER_TILES_Y);

                        // Framebuffer coordinates of that point: x and y map
                        // linearly from [-1, 1], with y growing downwards
                        // (the projection already carries the flip).
                        const float frag_x = (ndc_x * 0.5f + 0.5f) * CHECK_RENDER_WIDTH;
                        const float frag_y = (ndc_y * 0.5f + 0.5f) * CHECK_RENDER_HEIGHT;

                        const Cluster_Coordinates expected{ tile_x, tile_y, slice };
                        const Cluster_Coordinates found = Find_cluster(mapping, CHECK_RENDER_WIDTH, CHECK_RENDER_HEIGHT, frag_x, frag_y, depth);

                        if (found.tile_x != expected.tile_x || found.tile_y != expected.tile_y || found.slice != expected.slice)
                            return std::string(_name) + ": the center of the cluster " + Describe(expected) + " is mapped to " + Describe(found);

                        const MathLib::Vector3 point = _point_at(ndc_x, ndc_y, depth);
                        const Cluster_AABB_GPU& box = boxes[Cluster_index(expected)];

                        const float slack = 1.0e-3f * (1.0f + std::abs(depth));

                        for (int axis = 0; axis < 3; ++axis)
                        {
                            if (point[axis] < box.min_point[axis] - slack || point[axis] > box.max_point[axis] + slack)
                                return std::string(_name) + ": the center of the cluster " + Describe(expected) + " lies outside the box built for it (axis "
                                    + std::to_string(axis) + ": " + std::to_string(point[axis]) + " not in [" + std::to_string(box.min_point[axis])
                                    + ", " + std::to_string(box.max_point[axis]) + "])";
                        }
                    }
                }
            }

            return {};
        }
    }

    std::string Verify_consistency()
    {
        // -- Slices: the mapping of a distance to a slice and the start
        //    distance of each slice are inverse of each other --
        for (const float near_plane : { 0.05f, 0.1f, 1.0f, 5.0f })
        {
            const std::string problem = Verify_slice_boundaries(near_plane);

            if (!problem.empty())
                return problem;
        }

        // -- Boxes: the cluster of a point is the cluster whose box holds it --
        const float aspect = CHECK_RENDER_WIDTH / CHECK_RENDER_HEIGHT;

        for (const float near_plane : { 0.1f, 1.0f, 5.0f })
        {
            // The perspective projection of the Extractor: infinite
            // reverse-Z, Y flipped.
            MathLib::Matrix4 perspective = MathLib::Mat4::Perspective_reverse_z_infinite(1.0471976f, aspect, near_plane);
            perspective[1][1] *= -1.0f;

            // ndc = (P00 * x / d, P11 * y / d), so x = ndc_x * d / P00.
            const std::string problem = Verify_boxes("perspective", perspective, near_plane,
                [&](float _ndc_x, float _ndc_y, float _depth)
                {
                    return MathLib::Vector3(_ndc_x * _depth / perspective[0][0], _ndc_y * _depth / perspective[1][1], -_depth);
                });

            if (!problem.empty())
                return "near " + std::to_string(near_plane) + ", " + problem;
        }

        {
            // The orthographic projection of the Extractor: reverse-Z
            // correction on top of the usual matrix, Y flipped.
            MathLib::Matrix4 orthographic = MathLib::Mat4::Reverse_z_correction()
                * MathLib::Mat4::Orthographic(-8.0f * aspect, 8.0f * aspect, -8.0f, 8.0f, 0.1f, 500.0f);
            orthographic[1][1] *= -1.0f;

            // ndc = (P00 * x, P11 * y), whatever the distance.
            const std::string problem = Verify_boxes("orthographic", orthographic, 0.1f,
                [&](float _ndc_x, float _ndc_y, float _depth)
                {
                    return MathLib::Vector3(_ndc_x / orthographic[0][0], _ndc_y / orthographic[1][1], -_depth);
                });

            if (!problem.empty())
                return problem;
        }

        return {};
    }

} // namespace Renderer_System::Cluster_Grid
