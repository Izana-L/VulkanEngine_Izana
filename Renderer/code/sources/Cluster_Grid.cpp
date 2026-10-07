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

                    const uint32_t index = tile_x + tile_y * CLUSTER_TILES_X + slice * CLUSTER_TILES_X * CLUSTER_TILES_Y;

                    _out_aabbs[index].min_point = MathLib::Vector4(box_min, 0.0f);
                    _out_aabbs[index].max_point = MathLib::Vector4(box_max, 0.0f);
                }
            }
        }
    }

} // namespace Renderer_System::Cluster_Grid
