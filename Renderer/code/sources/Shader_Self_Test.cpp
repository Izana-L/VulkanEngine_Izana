#include <Shader_Self_Test.hpp>

#include <Cluster_Grid.hpp>
#include <Renderer_Limits.hpp>
#include <RenderPacket.hpp>
#include <Vulkan_Barrier.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Compute_Pipeline.hpp>
#include <Vulkan_Descriptor_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <Matrix4.hpp>
#include <Vector3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace Renderer_System::Shader_Self_Test
{

    namespace
    {
        constexpr uint32_t MAX_CASES = GPU_SELFTEST_MAX_CASES;

        // EXACT mirror of Self_Test_Input and Self_Test_Output in
        // selftest.comp, std430. Every member is four 32-bit values or an
        // array of them, so the C++ and GLSL offsets agree without padding
        // rules to remember; the static_asserts pin them.
        struct Self_Test_Input
        {
            uint32_t cluster_grid[4];                       // xyz = tiles X, tiles Y, slices
            float    cluster_params[4];                     // xy = render size, z = slice scale, w = slice bias
            float    frustum_planes[FRUSTUM_PLANE_COUNT][4];
            uint32_t case_counts[4];                        // x = cluster cases, y = ellipsoid cases, z = normalize cases
            float    cluster_cases[MAX_CASES][4];           // xy = fragment coordinates, z = view depth
            float    ellipsoid_cases[MAX_CASES * 5][4];     // per case: the four columns of the model, then the local sphere
            float    normalize_cases[MAX_CASES][4];         // xyz = vector
        };

        struct Self_Test_Output
        {
            uint32_t cluster_results[MAX_CASES][4];         // tile x, tile y, slice, cluster index
            uint32_t ellipsoid_results[MAX_CASES][4];       // x = 1 when the ellipsoid is kept
            float    normalize_results[MAX_CASES][4];       // xyz = Safe_normalize(vector), w = 1 when finite
        };

        static_assert(offsetof(Self_Test_Input, cluster_params) == 16, "Self_Test_Input breaks the std430 layout of selftest.comp");
        static_assert(offsetof(Self_Test_Input, frustum_planes) == 32, "Self_Test_Input breaks the std430 layout of selftest.comp");
        static_assert(offsetof(Self_Test_Input, case_counts) == 32 + 16 * FRUSTUM_PLANE_COUNT, "Self_Test_Input breaks the std430 layout of selftest.comp");
        static_assert(offsetof(Self_Test_Input, cluster_cases) == 48 + 16 * FRUSTUM_PLANE_COUNT, "Self_Test_Input breaks the std430 layout of selftest.comp");
        static_assert(offsetof(Self_Test_Input, ellipsoid_cases) == offsetof(Self_Test_Input, cluster_cases) + 16 * MAX_CASES, "Self_Test_Input breaks the std430 layout of selftest.comp");
        static_assert(offsetof(Self_Test_Input, normalize_cases) == offsetof(Self_Test_Input, ellipsoid_cases) + 16 * 5 * MAX_CASES, "Self_Test_Input breaks the std430 layout of selftest.comp");
        static_assert(sizeof(Self_Test_Input) == offsetof(Self_Test_Input, normalize_cases) + 16 * MAX_CASES, "Self_Test_Input breaks the std430 layout of selftest.comp");

        static_assert(offsetof(Self_Test_Output, ellipsoid_results) == 16 * MAX_CASES, "Self_Test_Output breaks the std430 layout of selftest.comp");
        static_assert(offsetof(Self_Test_Output, normalize_results) == 32 * MAX_CASES, "Self_Test_Output breaks the std430 layout of selftest.comp");
        static_assert(sizeof(Self_Test_Output) == 48 * MAX_CASES, "Self_Test_Output breaks the std430 layout of selftest.comp");

        // The one set of the pipeline: the two buffers.
        constexpr std::array<Vulkan_Descriptor_Utils::Layout_Binding, 2> SELF_TEST_BINDINGS = { {
            { GPU_BINDING_SELFTEST_INPUT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
            { GPU_BINDING_SELFTEST_OUTPUT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT } } };

        // ── Cases ─────────────────────────────────────────────────

        // Frame the cluster mapping is tested for: not square, so a swapped
        // axis is noticed.
        constexpr float RENDER_WIDTH = 1280.0f;
        constexpr float RENDER_HEIGHT = 720.0f;
        constexpr float CLUSTER_NEAR_PLANE = 0.25f;

        // Distance from a slice boundary, relative, at which its two sides
        // are tested: far beyond the error of the GPU logarithm, well
        // inside a slice.
        constexpr float BOUNDARY_MARGIN = 1.0e-3f;

        // Expected values of the cases, computed with the C++ twins.
        struct Expected_Cases
        {
            Cluster_Grid::Slice_Mapping                 mapping;
            std::vector<Cluster_Grid::Cluster_Coordinates> clusters;
            std::vector<bool>                           ellipsoids_kept;
            Frustum                                     frustum;
            std::vector<std::array<float, 3>>           normalized;
        };

        void Add_cluster_case(Self_Test_Input& _input, Expected_Cases& _expected, float _frag_x, float _frag_y, float _depth)
        {
            const uint32_t index = _input.case_counts[0]++;

            _input.cluster_cases[index][0] = _frag_x;
            _input.cluster_cases[index][1] = _frag_y;
            _input.cluster_cases[index][2] = _depth;
            _input.cluster_cases[index][3] = 0.0f;

            _expected.clusters.push_back(Cluster_Grid::Find_cluster(_expected.mapping, RENDER_WIDTH, RENDER_HEIGHT, _frag_x, _frag_y, _depth));
        }

        void Build_cluster_cases(Self_Test_Input& _input, Expected_Cases& _expected)
        {
            _expected.mapping = Cluster_Grid::Make_slice_mapping(CLUSTER_NEAR_PLANE);

            _input.cluster_grid[0] = CLUSTER_TILES_X;
            _input.cluster_grid[1] = CLUSTER_TILES_Y;
            _input.cluster_grid[2] = CLUSTER_SLICES;
            _input.cluster_grid[3] = 0;

            _input.cluster_params[0] = RENDER_WIDTH;
            _input.cluster_params[1] = RENDER_HEIGHT;
            _input.cluster_params[2] = _expected.mapping.scale;
            _input.cluster_params[3] = _expected.mapping.bias;

            const float tile_width = RENDER_WIDTH / static_cast<float>(CLUSTER_TILES_X);
            const float tile_height = RENDER_HEIGHT / static_cast<float>(CLUSTER_TILES_Y);

            // The middle of every slice, in a tile that changes from slice
            // to slice (centers of tiles: never on a tile edge).
            for (uint32_t slice = 0; slice < CLUSTER_SLICES; ++slice)
            {
                const float depth = std::sqrt(Cluster_Grid::Slice_start_distance(_expected.mapping, slice)
                                              * Cluster_Grid::Slice_start_distance(_expected.mapping, slice + 1));
                const uint32_t tile_x = (slice * 7u) % CLUSTER_TILES_X;
                const uint32_t tile_y = (slice * 5u) % CLUSTER_TILES_Y;

                Add_cluster_case(_input, _expected, (static_cast<float>(tile_x) + 0.5f) * tile_width,
                                 (static_cast<float>(tile_y) + 0.5f) * tile_height, depth);
            }

            // Both sides of every slice boundary.
            for (uint32_t slice = 1; slice < CLUSTER_SLICES; ++slice)
            {
                const float start = Cluster_Grid::Slice_start_distance(_expected.mapping, slice);

                Add_cluster_case(_input, _expected, 0.5f, 0.5f, start * (1.0f + BOUNDARY_MARGIN));
                Add_cluster_case(_input, _expected, 0.5f, 0.5f, start * (1.0f - BOUNDARY_MARGIN));
            }

            // Closer than the near plane, and beyond the last boundary.
            Add_cluster_case(_input, _expected, 640.0f, 360.0f, CLUSTER_NEAR_PLANE * 0.5f);
            Add_cluster_case(_input, _expected, 640.0f, 360.0f, 1.0e6f);

            // The corners of the screen, a pixel past them (the tile is
            // clamped to the grid), and the first pixel of the second tile.
            Add_cluster_case(_input, _expected, 0.5f, 0.5f, 5.0f);
            Add_cluster_case(_input, _expected, RENDER_WIDTH - 0.5f, RENDER_HEIGHT - 0.5f, 5.0f);
            Add_cluster_case(_input, _expected, RENDER_WIDTH + 10.0f, RENDER_HEIGHT + 10.0f, 5.0f);
            Add_cluster_case(_input, _expected, tile_width + 0.5f, tile_height + 0.5f, 5.0f);
        }

        // World space culling planes of a camera at the origin looking down
        // -Z (60 degrees, 16:9, near plane 0.1), unit normals pointing
        // inside: the shape the Extractor builds (Make_camera_frustum).
        Frustum Make_test_frustum()
        {
            const float tan_half_height = std::tan(1.0471976f * 0.5f);
            const float tan_half_width = tan_half_height * (16.0f / 9.0f);

            const MathLib::Vector3 right(1.0f, 0.0f, 0.0f);
            const MathLib::Vector3 up(0.0f, 1.0f, 0.0f);
            const MathLib::Vector3 forward(0.0f, 0.0f, -1.0f);

            // Planes through the origin: xyz = unit normal, w = 0.
            const auto side_plane = [](const MathLib::Vector3& _normal)
                {
                    return MathLib::Vector4(MathLib::Vec3::Normalize(_normal), 0.0f);
                };

            Frustum frustum;
            frustum.planes[0] = side_plane(right + forward * tan_half_width);      // left
            frustum.planes[1] = side_plane(-right + forward * tan_half_width);     // right
            frustum.planes[2] = side_plane(up + forward * tan_half_height);        // bottom
            frustum.planes[3] = side_plane(-up + forward * tan_half_height);       // top

            // Near plane through the point 0.1 ahead of the eye: w = -dot(forward, forward * 0.1).
            frustum.planes[4] = MathLib::Vector4(forward, -0.1f);

            return frustum;
        }

        // Signed distance of the ellipsoid to the closest plane, by the
        // formula of the culling: how far from being decided it is. Cases
        // whose margin is tiny could be decided either way by a different
        // rounding of the same formula, so they are not tested.
        float Culling_margin(const Frustum& _frustum, const MathLib::Matrix4& _model, const MathLib::Vector4& _sphere)
        {
            const MathLib::Vector3 center = MathLib::Vector3(_model * MathLib::Vector4(MathLib::Vector3(_sphere), 1.0f));

            float margin = std::numeric_limits<float>::max();

            for (const MathLib::Vector4& plane : _frustum.planes)
            {
                const MathLib::Vector3 normal(plane);
                const float distance = MathLib::Vec3::Dot(normal, center) + plane.w;
                const float extent = Frustum::Ellipsoid_extent(_model, _sphere.w, normal);

                margin = std::min(margin, std::abs(distance + extent));
            }

            return margin;
        }

        // Models with shear, mirroring and non-uniform scale (the
        // transforms the ellipsoid test exists for), spread around the
        // frustum so about half are kept. Deterministic: the same cases on
        // every run.
        void Build_ellipsoid_cases(Self_Test_Input& _input, Expected_Cases& _expected)
        {
            using namespace MathLib;

            _expected.frustum = Make_test_frustum();

            for (uint32_t plane = 0; plane < FRUSTUM_PLANE_COUNT; ++plane)
            {
                for (uint32_t component = 0; component < 4; ++component)
                    _input.frustum_planes[plane][component] = _expected.frustum.planes[plane][static_cast<int>(component)];
            }

            std::mt19937 generator(20240607u);
            const auto random = [&](float _min, float _max) { return std::uniform_real_distribution<float>(_min, _max)(generator); };

            constexpr uint32_t CASES = 96;
            constexpr uint32_t PER_OUTCOME = CASES * 2 / 3;   // neither outcome may fill the table

            uint32_t kept = 0;
            uint32_t culled = 0;

            for (uint32_t attempt = 0; attempt < 100000 && kept + culled < CASES; ++attempt)
            {
                Matrix4 model = Mat4::Translation(random(-40.0f, 40.0f), random(-40.0f, 40.0f), random(-80.0f, 10.0f));
                model = model * Mat4::RotationX(random(0.0f, 6.28f)) * Mat4::RotationY(random(0.0f, 6.28f));

                // A negative X scale mirrors the model; the rotation after
                // the scale shears it.
                model = model * Mat4::Scale(random(-3.0f, 3.0f), random(0.2f, 3.0f), random(0.2f, 3.0f)) * Mat4::RotationZ(random(0.0f, 6.28f));

                const Vector4 sphere(random(-1.0f, 1.0f), random(-1.0f, 1.0f), random(-1.0f, 1.0f), random(0.1f, 2.0f));

                if (Culling_margin(_expected.frustum, model, sphere) < 1.0e-2f)
                    continue;

                const bool is_kept = _expected.frustum.Intersects_ellipsoid(model, sphere);

                if ((is_kept && kept >= PER_OUTCOME) || (!is_kept && culled >= PER_OUTCOME))
                    continue;

                (is_kept ? kept : culled) += 1;

                const uint32_t index = _input.case_counts[1]++;

                for (uint32_t column = 0; column < 4; ++column)
                {
                    for (uint32_t component = 0; component < 4; ++component)
                        _input.ellipsoid_cases[index * 5 + column][component] = model[static_cast<int>(column)][static_cast<int>(component)];
                }

                for (uint32_t component = 0; component < 4; ++component)
                    _input.ellipsoid_cases[index * 5 + 4][component] = sphere[static_cast<int>(component)];

                _expected.ellipsoids_kept.push_back(is_kept);
            }
        }

        // Vectors Safe_normalize must handle: the zero vector above all
        // (normalize(0) is NaN), and ordinary ones, including very short.
        void Build_normalize_cases(Self_Test_Input& _input, Expected_Cases& _expected)
        {
            const std::array<std::array<float, 3>, 7> vectors = { {
                { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, -2.0f, 0.0f }, { 3.0f, 4.0f, 0.0f },
                { -5.0f, 12.0f, 0.0f }, { 1.0e-3f, 0.0f, 0.0f }, { 0.5f, 0.5f, -0.5f } } };

            for (const std::array<float, 3>& vector : vectors)
            {
                const uint32_t index = _input.case_counts[2]++;

                for (uint32_t component = 0; component < 3; ++component)
                    _input.normalize_cases[index][component] = vector[component];

                // The definition of Safe_normalize: v * inversesqrt(max(dot(v, v), 1e-30)).
                const float squared = vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2];
                const float scale = 1.0f / std::sqrt(std::max(squared, 1.0e-30f));

                _expected.normalized.push_back({ vector[0] * scale, vector[1] * scale, vector[2] * scale });
            }
        }

        // ── Comparison ────────────────────────────────────────────

        // How many disagreements of each kind the report lists in full.
        constexpr uint32_t REPORTED_PER_KIND = 4;

        std::string Compare(const Self_Test_Input& _input, const Self_Test_Output& _output, const Expected_Cases& _expected)
        {
            std::ostringstream report;

            uint32_t cluster_bad = 0;

            for (uint32_t i = 0; i < _input.case_counts[0]; ++i)
            {
                const Cluster_Grid::Cluster_Coordinates& expected = _expected.clusters[i];
                const uint32_t* result = _output.cluster_results[i];

                if (result[0] == expected.tile_x && result[1] == expected.tile_y && result[2] == expected.slice
                    && result[3] == Cluster_Grid::Cluster_index(expected))
                    continue;

                if (cluster_bad++ < REPORTED_PER_KIND)
                {
                    report << "\n  cluster case " << i << ": fragment (" << _input.cluster_cases[i][0] << ", " << _input.cluster_cases[i][1]
                        << ") at depth " << _input.cluster_cases[i][2] << ": the shader found tile (" << result[0] << ", " << result[1]
                        << ") slice " << result[2] << " index " << result[3] << ", the C++ twin tile (" << expected.tile_x << ", "
                        << expected.tile_y << ") slice " << expected.slice << " index " << Cluster_Grid::Cluster_index(expected);
                }
            }

            if (cluster_bad > 0)
                report << "\n  " << cluster_bad << " of " << _input.case_counts[0] << " cluster cases differ (cluster_math.glsl against Cluster_Grid::Find_cluster)";

            uint32_t ellipsoid_bad = 0;

            for (uint32_t i = 0; i < _input.case_counts[1]; ++i)
            {
                const bool shader_kept = _output.ellipsoid_results[i][0] != 0;

                if (shader_kept == _expected.ellipsoids_kept[i])
                    continue;

                if (ellipsoid_bad++ < REPORTED_PER_KIND)
                    report << "\n  ellipsoid case " << i << ": the shader " << (shader_kept ? "keeps" : "culls") << " it, the C++ test "
                        << (_expected.ellipsoids_kept[i] ? "keeps" : "culls") << " it";
            }

            if (ellipsoid_bad > 0)
                report << "\n  " << ellipsoid_bad << " of " << _input.case_counts[1]
                    << " ellipsoid cases differ (bounding_math.glsl against Frustum::Intersects_ellipsoid)";

            uint32_t normalize_bad = 0;

            for (uint32_t i = 0; i < _input.case_counts[2]; ++i)
            {
                const float* result = _output.normalize_results[i];
                const std::array<float, 3>& expected = _expected.normalized[i];

                bool same = result[3] == 1.0f;

                for (uint32_t component = 0; component < 3; ++component)
                    same = same && std::abs(result[component] - expected[component]) < 1.0e-4f;

                if (same)
                    continue;

                if (normalize_bad++ < REPORTED_PER_KIND)
                    report << "\n  normalize case " << i << ": (" << _input.normalize_cases[i][0] << ", " << _input.normalize_cases[i][1] << ", "
                        << _input.normalize_cases[i][2] << ") gave (" << result[0] << ", " << result[1] << ", " << result[2] << ")"
                        << (result[3] == 1.0f ? "" : ", not finite") << ", expected (" << expected[0] << ", " << expected[1] << ", " << expected[2] << ")";
            }

            if (normalize_bad > 0)
                report << "\n  " << normalize_bad << " of " << _input.case_counts[2] << " normalize cases differ (safe_math.glsl)";

            return report.str();
        }

        // ── Vulkan objects of the test ────────────────────────────

        // Everything the test creates, released in reverse order when it
        // goes out of scope. The device is idle with respect to the test by
        // then (Upload_Context::Run waits for it), except when the context
        // reports itself unusable: the GPU may still be using the buffers,
        // so they are leaked instead of freed under it.
        struct Resources
        {
            VkDevice                                 device = VK_NULL_HANDLE;
            VmaAllocator                             allocator = VK_NULL_HANDLE;
            const Upload_Context*                    context = nullptr;

            VkDescriptorSetLayout                    set_layout = VK_NULL_HANDLE;
            VkPipelineLayout                         pipeline_layout = VK_NULL_HANDLE;
            VkDescriptorPool                         pool = VK_NULL_HANDLE;
            Vulkan_Buffer_Utils::Buffer_Allocation   input;
            Vulkan_Buffer_Utils::Buffer_Allocation   output;
            Vulkan_Buffer_Utils::Buffer_Allocation   readback;

            Resources(VkDevice _device, VmaAllocator _allocator, const Upload_Context& _context)
                : device(_device), allocator(_allocator), context(&_context)
            {
            }

            Resources(const Resources&) = delete;
            Resources& operator=(const Resources&) = delete;

            ~Resources()
            {
                if (!context->Is_usable())
                    return;

                Vulkan_Buffer_Utils::Destroy_buffer(allocator, readback);
                Vulkan_Buffer_Utils::Destroy_buffer(allocator, output);
                Vulkan_Buffer_Utils::Destroy_buffer(allocator, input);

                if (pool != VK_NULL_HANDLE)
                    vkDestroyDescriptorPool(device, pool, nullptr);

                if (pipeline_layout != VK_NULL_HANDLE)
                    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);

                if (set_layout != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            }
        };
    }

    std::string Run(const Vulkan_Device& _device, VmaAllocator _allocator, VkPipelineCache _pipeline_cache,
                    Upload_Context& _upload_context)
    {
        const VkDevice device = _device.Get_logical_device_handle();

        // -- Cases --
        auto input = std::make_unique<Self_Test_Input>();
        std::memset(input.get(), 0, sizeof(Self_Test_Input));

        Expected_Cases expected;

        Build_cluster_cases(*input, expected);
        Build_ellipsoid_cases(*input, expected);
        Build_normalize_cases(*input, expected);

        // -- Vulkan objects --
        Resources resources(device, _allocator, _upload_context);

        resources.set_layout = Vulkan_Descriptor_Utils::Create_set_layout(device, SELF_TEST_BINDINGS, "Shader self-test: set layout");

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &resources.set_layout;

        VK_CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &resources.pipeline_layout), "Shader self-test: pipeline layout");

        resources.pool = Vulkan_Descriptor_Utils::Pool_Builder().Add_sets(SELF_TEST_BINDINGS).Create(device, 0, "Shader self-test: descriptor pool");

        const VkDescriptorSet set = Vulkan_Descriptor_Utils::Allocate_set(device, resources.pool, resources.set_layout, "Shader self-test: descriptor set");

        // The cases are written by the CPU; the results are written by the
        // GPU into device memory and copied to a host-readable buffer, the
        // way the frame statistics are read back.
        resources.input = Vulkan_Buffer_Utils::Create_buffer(_allocator, sizeof(Self_Test_Input),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);
        resources.output = Vulkan_Buffer_Utils::Create_buffer(_allocator, sizeof(Self_Test_Output),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only);
        resources.readback = Vulkan_Buffer_Utils::Create_buffer(_allocator, sizeof(Self_Test_Output),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, Vulkan_Buffer_Utils::Buffer_Access::Gpu_To_Cpu, true);

        Vulkan_Descriptor_Utils::Descriptor_Writer writer(SELF_TEST_BINDINGS);
        writer.Write_buffer(set, GPU_BINDING_SELFTEST_INPUT, resources.input)
              .Write_buffer(set, GPU_BINDING_SELFTEST_OUTPUT, resources.output);
        writer.Update(device);

        Vulkan_Buffer_Utils::Upload_to_buffer(_allocator, resources.input, input.get(), sizeof(Self_Test_Input));

        const Vulkan_Compute_Pipeline pipeline(_device, _pipeline_cache, resources.pipeline_layout,
            "..\\..\\Renderer\\shaders\\compiled\\selftest.comp.spv");

        // -- Dispatch and readback, waited for on the CPU --
        _upload_context.Run([&](VkCommandBuffer _command_buffer)
            {
                vkCmdBindDescriptorSets(_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipeline_layout, 0, 1, &set, 0, nullptr);
                pipeline.Dispatch_groups(_command_buffer, Dispatch_group_count(MAX_CASES, GPU_SELFTEST_GROUP_SIZE));

                Vulkan_Barrier::Record_memory_barrier(_command_buffer,
                    { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT },
                    { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT });

                const VkBufferCopy copy{ 0, 0, sizeof(Self_Test_Output) };
                vkCmdCopyBuffer(_command_buffer, resources.output.buffer, resources.readback.buffer, 1, &copy);

                Vulkan_Barrier::Record_memory_barrier(_command_buffer,
                    { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT },
                    { VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT });
            });

        auto output = std::make_unique<Self_Test_Output>();
        Vulkan_Buffer_Utils::Read_from_buffer(_allocator, resources.readback, output.get(), sizeof(Self_Test_Output));

        return Compare(*input, *output, expected);
    }

} // namespace Renderer_System::Shader_Self_Test
