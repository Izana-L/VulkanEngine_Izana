#include <Renderer.hpp>
#include <Vulkan_Utils.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Cluster_Grid.hpp>
#include <Vertex_Packing.hpp>
#include <MathConstants.hpp>
#include <Matrix4.hpp>
#include <Window.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <array>
#include <cassert>
#include <string>
#include <utility>

namespace Renderer_System
{

    namespace
    {
        // Frames a retired semaphore has to survive before it is destroyed
        // when no present fence can prove its present completed: every
        // frame slot plus every swapchain image has cycled by then.
        constexpr uint32_t RETIRED_SYNC_GRACE_FRAMES = 8;

        // How long a shutdown or a recreation waits for a present fence
        // before giving up on it (nanoseconds). A driver never signaling a
        // present fence is a driver fault; the wait must not hang forever.
        constexpr uint64_t PRESENT_FENCE_TIMEOUT_NS = 1'000'000'000ull;

        // local_size_x of cluster_lights.comp and cull_objects.comp.
        constexpr uint32_t CLUSTER_GROUP_SIZE = 64;
        constexpr uint32_t CULL_GROUP_SIZE = 64;

        // Stride of every indirect command buffer: tightly packed
        // VkDrawIndexedIndirectCommand, the layout cull_objects.comp writes.
        constexpr uint32_t DRAW_COMMAND_STRIDE = static_cast<uint32_t>(sizeof(VkDrawIndexedIndirectCommand));

        // vkCmdUpdateBuffer accepts at most 65536 bytes per call.
        constexpr VkDeviceSize UPDATE_BUFFER_MAX_BYTES = 65536;

        // Segments and rings of the unit sphere of the bounding volume view.
        constexpr uint32_t BOUNDS_SPHERE_SEGMENTS = 16;
        constexpr uint32_t BOUNDS_SPHERE_RINGS = 8;

        // Time between two statistics prints.
        constexpr std::chrono::milliseconds STATISTICS_PRINT_INTERVAL{ 1000 };

        // Scope names are string literals, but the same literal may live at
        // different addresses: compared by content. nullptr (no parent)
        // only equals nullptr.
        bool Same_scope_name(const char* _a, const char* _b)
        {
            if (_a == _b)
                return true;

            if (_a == nullptr || _b == nullptr)
                return false;

            return std::strcmp(_a, _b) == 0;
        }

        static_assert(FRUSTUM_PLANE_COUNT == CoreTypes::Frustum::PLANE_COUNT,
            "Frame_UBO::frustum_planes and CoreTypes::Frustum must hold the same planes");

        VkDeviceSize Align_up(VkDeviceSize _value, VkDeviceSize _alignment)
        {
            return (_value + _alignment - 1) / _alignment * _alignment;
        }

#ifndef NDEBUG
        // Debug self-check of the culling test (CoreTypes::Frustum::
        // Intersects_ellipsoid), on a case whose result is known: a mesh
        // bounding sphere of radius 1 at the origin under a parent scaled
        // (2, 1, 1) and a child rotated 45 degrees about Z. The product has
        // shear; the longest column of its 3x3 measures sqrt(2.5) = 1.581,
        // while the ellipsoid reaches exactly 2 along world X. A plane
        // x >= -1.9 must keep the object centered at x = -3.8 (it reaches
        // x = -1.8) and cull the one centered at x = -4.0 (it reaches
        // x = -2.0). The longest column would cull both.
        //
        // cull_objects.comp evaluates the same formula
        // (mesh_table.glsl); a change to one side that breaks this case
        // makes the two cullings diverge. Throws std::logic_error.
        void Check_ellipsoid_culling()
        {
            using namespace MathLib;

            const Matrix4 sheared = Mat4::Scale(2.0f, 1.0f, 1.0f) * Mat4::RotationZ(Constants::QUARTER_PI);
            const Vector4 unit_sphere(0.0f, 0.0f, 0.0f, 1.0f);

            const float extent_x = CoreTypes::Frustum::Ellipsoid_extent(sheared, unit_sphere.w, Vector3(1.0f, 0.0f, 0.0f));

            // Only plane 0 is set: zero planes contain everything.
            CoreTypes::Frustum frustum;
            frustum.planes[0] = Vector4(1.0f, 0.0f, 0.0f, 1.9f);

            const bool partly_inside_kept = frustum.Intersects_ellipsoid(Mat4::Translation(-3.8f, 0.0f, 0.0f) * sheared, unit_sphere);
            const bool outside_culled = !frustum.Intersects_ellipsoid(Mat4::Translation(-4.0f, 0.0f, 0.0f) * sheared, unit_sphere);

            if (std::abs(extent_x - 2.0f) > 1.0e-4f || !partly_inside_kept || !outside_culled)
            {
                throw std::logic_error("Renderer: the ellipsoid culling test failed its known case (extent " + std::to_string(extent_x) +
                                       ", expected 2; partly visible object kept: " + (partly_inside_kept ? "yes" : "no") +
                                       "; hidden object culled: " + (outside_culled ? "yes" : "no") + ")");
            }
        }
#endif

        // Unit sphere (radius 1, centered at the origin) as a latitude /
        // longitude grid. Only positions matter to bounds.vert, which maps
        // it onto each object's bounding ellipsoid; the other attributes
        // get neutral values.
        CoreTypes::MeshData Build_unit_sphere(uint32_t _segments, uint32_t _rings)
        {
            CoreTypes::MeshData mesh;
            mesh.index_type = CoreTypes::Index_Type::UINT32;

            for (uint32_t ring = 0; ring <= _rings; ++ring)
            {
                const float phi = MathLib::Constants::PI * static_cast<float>(ring) / static_cast<float>(_rings);

                for (uint32_t segment = 0; segment <= _segments; ++segment)
                {
                    const float theta = MathLib::Constants::TWO_PI * static_cast<float>(segment) / static_cast<float>(_segments);

                    CoreTypes::Vertex_Static_Mesh_CPU vertex{};
                    vertex.position = { std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta) };
                    vertex.normal = vertex.position;
                    vertex.tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
                    vertex.uv = { static_cast<float>(segment) / static_cast<float>(_segments),
                                  static_cast<float>(ring) / static_cast<float>(_rings) };
                    vertex.color = { 1.0f, 1.0f, 1.0f, 1.0f };

                    mesh.vertices.push_back(vertex);
                }
            }

            const uint32_t stride = _segments + 1;

            for (uint32_t ring = 0; ring < _rings; ++ring)
            {
                for (uint32_t segment = 0; segment < _segments; ++segment)
                {
                    const uint32_t a = ring * stride + segment;
                    const uint32_t b = a + stride;

                    mesh.indices.insert(mesh.indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
                }
            }

            return mesh;
        }
    }

    // =========================================================
    // Debug names
    // =========================================================

    const char* To_string(Light_Culling_Mode _mode)
    {
        switch (_mode)
        {
        case Light_Culling_Mode::Clustered:   return "clustered";
        case Light_Culling_Mode::Brute_Force: return "brute force (every light)";
        default:                              return "unknown";
        }
    }

    const char* To_string(Cluster_Debug_View _view)
    {
        switch (_view)
        {
        case Cluster_Debug_View::None:          return "none";
        case Cluster_Debug_View::Light_Heatmap: return "light count heatmap";
        case Cluster_Debug_View::Depth_Slices:  return "depth slices";
        case Cluster_Debug_View::Clusters:      return "clusters (tiles x slices)";
        default:                                return "unknown";
        }
    }

    const char* To_string(Opaque_Draw_Path _path)
    {
        switch (_path)
        {
        case Opaque_Draw_Path::Direct:       return "direct draws";
        case Opaque_Draw_Path::Cpu_Indirect: return "indirect, commands written by the CPU";
        case Opaque_Draw_Path::Gpu_Indirect: return "indirect count, commands written by the GPU (no culling)";
        case Opaque_Draw_Path::Gpu_Culled:   return "indirect count, GPU frustum culling";
        default:                             return "unknown";
        }
    }

    // =========================================================
    // Pipeline configurations
    // =========================================================

    Pipeline_Config Renderer::Make_opaque_config()
    {
        Pipeline_Config config;
        config.vertex_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.vert.spv";
        config.fragment_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.frag.spv";
        config.subpass = Render_Subpass::Opaque;
        config.color_attachment_count = 1;
        config.color_blend[0] = Color_Blend_State{};   // no blending
        return config;
    }

    Pipeline_Config Renderer::Make_bounds_config(bool _wireframe)
    {
        Pipeline_Config config;
        config.vertex_shader_path = "..\\..\\Renderer\\shaders\\compiled\\bounds.vert.spv";
        config.fragment_shader_path = "..\\..\\Renderer\\shaders\\compiled\\bounds.frag.spv";

        // Drawn after the OIT composite, over the final color, and tested
        // against the opaque depth.
        config.subpass = Render_Subpass::Composite;
        config.color_attachment_count = 1;

        if (_wireframe)
        {
            // Edges only: every volume stays readable where many overlap.
            config.polygon_mode = VK_POLYGON_MODE_LINE;
            config.color_blend[0] = Color_Blend_State{};
        }
        else
        {
            // Without fillModeNonSolid: faint filled volumes, standard
            // "over" blending with the alpha of bounds.frag.
            config.polygon_mode = VK_POLYGON_MODE_FILL;
            config.color_blend[0] = Color_Blend_State::Over();
        }

        return config;
    }

    Pipeline_Config Renderer::Make_transparent_config()
    {
        // Same vertex shader and lighting as the opaque pass
        // (common/shading.glsl); mesh_oit.frag writes the two OIT targets
        // of the transparent subpass instead of a color. Depth writes are
        // disabled at draw time through dynamic state, not here.
        Pipeline_Config config;
        config.vertex_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.vert.spv";
        config.fragment_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh_oit.frag.spv";
        config.subpass = Render_Subpass::Transparent;
        config.color_attachment_count = 2;

        // Accumulation: every fragment adds (color * alpha * w, alpha * w).
        Color_Blend_State& accumulation = config.color_blend[0];
        accumulation.blend_enable = true;
        accumulation.src_color_blend_factor = VK_BLEND_FACTOR_ONE;
        accumulation.dst_color_blend_factor = VK_BLEND_FACTOR_ONE;
        accumulation.color_blend_op = VK_BLEND_OP_ADD;
        accumulation.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        accumulation.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        accumulation.alpha_blend_op = VK_BLEND_OP_ADD;

        // Revealage: every fragment outputs its alpha as color and the
        // target is multiplied by (1 - alpha). One channel: the alpha
        // factors do not apply to it.
        Color_Blend_State& revealage = config.color_blend[1];
        revealage.blend_enable = true;
        revealage.src_color_blend_factor = VK_BLEND_FACTOR_ZERO;
        revealage.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        revealage.color_blend_op = VK_BLEND_OP_ADD;
        revealage.src_alpha_blend_factor = VK_BLEND_FACTOR_ZERO;
        revealage.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        revealage.alpha_blend_op = VK_BLEND_OP_ADD;

        return config;
    }

    Pipeline_Config Renderer::Make_composite_config()
    {
        // One full-screen triangle generated from gl_VertexIndex: no vertex
        // input. oit_composite.frag outputs the weighted average color of
        // the transparent layers with alpha = 1 - revealage, blended "over"
        // the opaque color:
        //   color = average * (1 - revealage) + opaque * revealage.
        Pipeline_Config config;
        config.vertex_shader_path = "..\\..\\Renderer\\shaders\\compiled\\oit_composite.vert.spv";
        config.fragment_shader_path = "..\\..\\Renderer\\shaders\\compiled\\oit_composite.frag.spv";
        config.subpass = Render_Subpass::Composite;
        config.vertex_input = Vertex_Input::None;
        config.color_attachment_count = 1;
        config.color_blend[0] = Color_Blend_State::Over();
        return config;
    }

    // =========================================================
    // Constructor
    // =========================================================

    Renderer::Renderer(const Platform::Window& _window, Validation_Mode _validation_mode)
                        : window(_window),
                        instance(_validation_mode, "Game", "Engine"),
                        surface(instance, _window),
                        device(instance, surface),
                        allocator(instance, device),
                        debug_utils(instance, device),
                        geometry_pool(allocator.Get_handle(), GEOMETRY_POOL_VERTICES, GEOMETRY_POOL_INDICES),
                        gpu_timer(device, FRAMES_IN_FLIGHT, GPU_TIMER_MAX_SCOPES),
                        swapchain(device, surface, _window, 3, false),
                        // The OIT formats are constants of Vulkan_OIT_Resources, which
                        // creates the targets with exactly those.
                        render_pass(device, swapchain.Get_image_format(), device.Find_supported_depth_format(),
                                    Vulkan_OIT_Resources::ACCUMULATION_FORMAT, Vulkan_OIT_Resources::REVEALAGE_FORMAT),
                        // The depth format is negotiated once, by the render pass; the
                        // depth image must use exactly that one.
                        depth_resources(device, allocator.Get_handle(), render_pass.Get_depth_format(), swapchain.Get_extent()),
                        oit_resources(device, allocator.Get_handle(), swapchain.Get_extent()),
                        framebuffers(device, render_pass, swapchain, depth_resources, oit_resources),
                        bindless_registry(device, BINDLESS_DESIRED_TEXTURES, BINDLESS_DESIRED_SAMPLERS),
                        pipeline_cache(device),
                        descriptor_layouts(device, bindless_registry.Get_layout()),
                        pipeline_layout(device, descriptor_layouts),
                        pipeline_registry(device, render_pass, pipeline_cache.Get_handle(), pipeline_layout.Get_handle()),
                        compute_pipeline_layout(device, descriptor_layouts, Pipeline_Kind::Compute, VK_SHADER_STAGE_COMPUTE_BIT, COMPUTE_PUSH_CONSTANT_SIZE),
                        procedural_pipeline(device, pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle(),"..\\..\\Renderer\\shaders\\compiled\\procedural.comp.spv"),
                        cluster_pipeline(device, pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle(), "..\\..\\Renderer\\shaders\\compiled\\cluster_lights.comp.spv"),
                        cull_pipeline(device, pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle(), "..\\..\\Renderer\\shaders\\compiled\\cull_objects.comp.spv"),
                        opaque_config(Make_opaque_config()),
                        transparent_config(Make_transparent_config()),
                        bounds_config(Make_bounds_config(device.Is_fill_mode_non_solid_enabled())),
                        composite_config(Make_composite_config()),
                        sampler_cache(device),
                        transfer_command_pool(device, 0, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT)
    {
        try
        {
#ifndef NDEBUG
            // The CPU culling of the transparent items and the GPU culling
            // of the opaque ones evaluate the same formula; its known case
            // is checked once per debug run.
            Check_ellipsoid_culling();
#endif

            // ── Bindless samplers ──────────────────────────────────────
            // Slot i of the sampler array receives the sampler of
            // Sampler_Preset i, so the preset a material stores is already
            // its bindless index. Written once, before any frame is
            // recorded: the only moment a slot can be written without
            // racing a frame in flight.
            for (uint32_t i = 0; i < static_cast<uint32_t>(CoreTypes::Sampler_Preset::Count); ++i)
            {
                const auto preset = static_cast<CoreTypes::Sampler_Preset>(i);
                bindless_registry.Set_sampler(i, sampler_cache.Get_sampler(Sampler_Cache::Get_preset_desc(preset)));
            }

            pipeline_registry.Warm_up(Build_pipeline_manifest());

            // Pure lookups: every pipeline already exists after the warm-up.
            opaque_pipeline_id = pipeline_registry.Get_id(opaque_config);
            transparent_pipeline_id = pipeline_registry.Get_id(transparent_config);
            bounds_pipeline_id = pipeline_registry.Get_id(bounds_config);
            composite_pipeline_id = pipeline_registry.Get_id(composite_config);

            // ── Frame resources ────────────────────────────────────────
            frames.reserve(FRAMES_IN_FLIGHT);
            for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
                frames.emplace_back(device, allocator.Get_handle());

            Create_image_sync();

            // ── Descriptor pool + sets ─────────────────────────────────
            Init_descriptor_pool();
            Init_descriptor_sets();
            Init_composite_input_set();

            // ── Transfer fence ─────────────────────────────────────────
            // NOT pre-signaled: Submit_and_wait_transfer resets it before every
            // submit, so its state at creation time is irrelevant.
            VkFenceCreateInfo transfer_fence_info{};
            transfer_fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

            VK_CHECK(vkCreateFence(device.Get_logical_device_handle(), &transfer_fence_info, nullptr, &transfer_fence),
                "Renderer: failed to create transfer fence");

            // ── Default textures ───────────────────────────────────────
            // Needs the transfer fence above. First upload of the session,
            // so the defaults take the reserved bindless slots.
            Upload_default_textures();

            // ── Material and mesh tables ───────────────────────────────
            // After the defaults: the default material samples
            // Default_Texture::White, and registration checks that its
            // slot exists. After the descriptor pool (per_material_set).
            // Before any mesh upload: uploads write the mesh table.
            Init_global_tables();

            // ── Compute pass resources ─────────────────────────────────
            // After the defaults (its bindless slot comes after theirs) and
            // after the descriptor pool (per_pass_set is allocated from it).
            Init_procedural_pass();

            // After Init_procedural_pass, which allocates per_pass_set.
            Init_light_clusters();

            // After the mesh table exists.
            Init_debug_meshes();

            Name_debug_objects();

            // Per-frame lists at their maximum size once, instead of
            // growing during the first frames.
            opaque_draws.reserve(MAX_OBJECTS);
            transparent_draws.reserve(MAX_OBJECTS);
            timer_frame.scopes.reserve(GPU_TIMER_MAX_SCOPES);
            statistics.scopes.reserve(GPU_TIMER_MAX_SCOPES);
            statistics.last_print = std::chrono::steady_clock::now();
        }
        catch (...)
        {
            // The destructor does not run for an object whose constructor
            // threw; the members are destroyed, the handles owned directly
            // by this class are not.
            Destroy_owned_handles();
            throw;
        }

        std::cout << "[Renderer] Initialized (" << swapchain.Get_image_count()
            << " swapchain images, " << FRAMES_IN_FLIGHT << " frames in flight, present fences "
            << (device.Is_swapchain_maintenance1_enabled() ? "on" : "off") << ").\n";
    }

    // =========================================================
    // Destructor
    // =========================================================

    Renderer::~Renderer()
    {
        // A device that cannot go idle at shutdown (device lost) is not a
        // reason to skip the cleanup that is still possible.
        const VkResult idle = vkDeviceWaitIdle(device.Get_logical_device_handle());
        if (idle != VK_SUCCESS)
            std::cerr << "[Renderer] vkDeviceWaitIdle failed at shutdown: " << Vulkan_Utils::Vk_result_to_string(idle) << "\n";

        Destroy_owned_handles();

        // Vulkan core objects are destroyed in reverse construction
        // order by their own destructors (RAII). sampler_cache destroys
        // all cached VkSampler handles in its own destructor.
        std::cout << "[Renderer] Destroyed.\n";
    }

    void Renderer::Destroy_owned_handles()
    {
        VkDevice dev = device.Get_logical_device_handle();

        // Presentation synchronization: wait for pending presents where a
        // fence can prove completion, then destroy everything.
        Retire_image_sync();
        Flush_retired_sync(true);

        // Assets first: they hold GPU buffers/images that may still be
        // referenced by in-flight command buffers otherwise. Mesh records
        // own no memory: their ranges are released with the Geometry_Pool.
        meshes.clear();
        retired_meshes.clear();
        textures.clear();
        procedural_image.reset();

        // Tolerate buffers that were never created (constructor failure
        // before Init_global_tables / Init_light_clusters).
        Vulkan_Buffer_Utils::Destroy_buffer(allocator.Get_handle(), material_buffer);
        Vulkan_Buffer_Utils::Destroy_buffer(allocator.Get_handle(), mesh_table_buffer);
        Vulkan_Buffer_Utils::Destroy_buffer(allocator.Get_handle(), cluster_aabb_buffer);
        registered_materials.clear();

        if (transfer_fence != VK_NULL_HANDLE) {
            vkDestroyFence(dev, transfer_fence, nullptr);
            transfer_fence = VK_NULL_HANDLE;
        }

        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(dev, descriptor_pool, nullptr);
            descriptor_pool = VK_NULL_HANDLE;
        }

        frames.clear();
    }

    // =========================================================
    // Per-image synchronization
    // =========================================================

    void Renderer::Create_image_sync()
    {
        VkDevice dev = device.Get_logical_device_handle();
        const bool use_present_fences = device.Is_swapchain_maintenance1_enabled();

        image_sync.assign(swapchain.Get_image_count(), Image_Sync{});

        for (Image_Sync& sync : image_sync)
        {
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            VK_CHECK(vkCreateSemaphore(dev, &semaphore_info, nullptr, &sync.render_finished),
                "Renderer: failed to create render_finished semaphore");

            if (use_present_fences)
            {
                // Not pre-signaled: only a real present signals it, and
                // present_pending records whether one is outstanding.
                VkFenceCreateInfo fence_info{};
                fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

                VK_CHECK(vkCreateFence(dev, &fence_info, nullptr, &sync.present_fence),
                    "Renderer: failed to create present fence");
            }
        }
    }

    void Renderer::Retire_image_sync()
    {
        VkDevice dev = device.Get_logical_device_handle();

        Retired_Sync retired;
        retired.frames_left = RETIRED_SYNC_GRACE_FRAMES;

        for (Image_Sync& sync : image_sync)
        {
            if (sync.present_fence != VK_NULL_HANDLE)
            {
                // With a present fence the completion of the last present
                // of this image is known exactly: wait for it, then both
                // objects can be destroyed immediately.
                bool completed = true;

                if (sync.present_pending)
                {
                    const VkResult wait = vkWaitForFences(dev, 1, &sync.present_fence, VK_TRUE, PRESENT_FENCE_TIMEOUT_NS);

                    if (wait == VK_SUCCESS)
                        sync.present_pending = false;
                    else
                    {
                        // Timeout or device loss: the fence may still be
                        // pending, so it must not be destroyed yet.
                        completed = false;
                        std::cerr << "[Renderer] Present fence not signaled before swapchain retirement: "
                            << Vulkan_Utils::Vk_result_to_string(wait) << "\n";
                    }
                }

                if (completed)
                {
                    vkDestroyFence(dev, sync.present_fence, nullptr);
                    vkDestroySemaphore(dev, sync.render_finished, nullptr);
                }
                else
                {
                    retired.fences.push_back(sync.present_fence);
                    retired.semaphores.push_back(sync.render_finished);
                }
            }
            else
            {
                // No present fence: nothing says when the presentation
                // engine is done waiting on this semaphore, so it is kept
                // alive for a grace period instead of destroyed now.
                retired.semaphores.push_back(sync.render_finished);
            }

            sync = Image_Sync{};
        }

        image_sync.clear();

        if (!retired.semaphores.empty() || !retired.fences.empty())
            retired_sync.push_back(std::move(retired));
    }

    void Renderer::Flush_retired_sync(bool _force)
    {
        VkDevice dev = device.Get_logical_device_handle();

        for (size_t i = 0; i < retired_sync.size(); )
        {
            Retired_Sync& retired = retired_sync[i];

            if (retired.frames_left > 0) --retired.frames_left;

            bool fences_done = true;

            for (VkFence fence : retired.fences)
            {
                if (_force)
                {
                    // Last chance: wait, bounded, then destroy regardless.
                    // A fence that never signals cannot be waited on forever
                    // at shutdown.
                    const VkResult wait = vkWaitForFences(dev, 1, &fence, VK_TRUE, PRESENT_FENCE_TIMEOUT_NS);
                    if (wait != VK_SUCCESS)
                        std::cerr << "[Renderer] Retired present fence still pending at shutdown: "
                            << Vulkan_Utils::Vk_result_to_string(wait) << "\n";
                }
                else if (vkGetFenceStatus(dev, fence) != VK_SUCCESS)
                {
                    fences_done = false;
                }
            }

            const bool ready = _force || (retired.frames_left == 0 && fences_done);

            if (!ready)
            {
                ++i;
                continue;
            }

            for (VkFence fence : retired.fences)
                vkDestroyFence(dev, fence, nullptr);

            for (VkSemaphore semaphore : retired.semaphores)
                vkDestroySemaphore(dev, semaphore, nullptr);

            retired_sync.erase(retired_sync.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // =========================================================
    // Upload
    // =========================================================

    Upload_Batch_Result Renderer::Upload_batch(const Upload_Batch& _batch)
    {
        Upload_Batch_Result result;

        const size_t mesh_count = _batch.meshes.size();
        const size_t texture_count = _batch.textures.size();

        // Nothing to do: don't allocate a command buffer for an empty batch.
        if (mesh_count == 0 && texture_count == 0)
            return result;

        // Validate the whole batch before touching the GPU, so a bad
        // element cannot leave half a batch uploaded.
        for (const CoreTypes::MeshData* mesh_data : _batch.meshes)
        {
            if (mesh_data == nullptr)
                throw std::invalid_argument("Upload_batch: null MeshData pointer");
            if (mesh_data->vertices.empty() || mesh_data->indices.empty())
                throw std::invalid_argument("Upload_batch: MeshData has no vertices or no indices");
            if (mesh_data->vertices.size() > UINT32_MAX || mesh_data->indices.size() > UINT32_MAX)
                throw std::invalid_argument("Upload_batch: MeshData has more than 2^32 - 1 vertices or indices");
        }

        for (const Texture_Upload& upload : _batch.textures)
        {
            if (upload.data == nullptr)
                throw std::invalid_argument("Upload_batch: null ImageData pointer");
            if (upload.data->pixels.empty() || upload.data->width == 0 || upload.data->height == 0)
                throw std::invalid_argument("Upload_batch: ImageData has no pixel data or zero dimensions");
        }

        // Every mesh of the batch needs an entry in the mesh table: its gpu
        // id is the entry index.
        if (meshes.size() + mesh_count > MAX_MESHES)
        {
            throw std::runtime_error("Upload_batch: the batch holds " + std::to_string(mesh_count) + " mesh(es) but the mesh table has " +
                                     std::to_string(MAX_MESHES - meshes.size()) + " free entries; raise MAX_MESHES in Frame_Data.hpp");
        }

        // Every texture of the batch needs a bindless slot after the
        // submit. Checked here, before recording, so a full array rejects
        // the batch while nothing has been created yet.
        const uint32_t free_texture_slots = bindless_registry.Get_free_texture_count();

        if (texture_count > free_texture_slots)
        {
            throw std::runtime_error("Upload_batch: the batch holds " + std::to_string(texture_count) +
                                     " texture(s) but the bindless texture array has only " + std::to_string(free_texture_slots) +
                                     " free slot(s); release unused slots or raise BINDLESS_DESIRED_TEXTURES, "
                                     "within the device limits logged at startup");
        }

        // Reserve before recording. Otherwise emplace_back can reallocate
        // mid-batch and move every Texture_GPU already recorded. Those
        // moves are safe (they null out the source), but reserving avoids
        // the churn, keeps the registries stable while the batch is built,
        // and makes the push_backs below unable to throw.
        meshes.reserve(meshes.size() + mesh_count);
        textures.reserve(textures.size() + texture_count);

        result.mesh_gpu_ids.reserve(mesh_count);
        result.texture_bindless_indices.reserve(texture_count);

        // Where this batch starts, so the post-submit pass below only
        // touches what this call added, and the failure path can undo it.
        const size_t first_mesh = meshes.size();
        const size_t first_texture = textures.size();

        Vulkan_Buffer_Utils::Buffer_Allocation staging{};
        VkCommandBuffer                        transfer_cmd = VK_NULL_HANDLE;

        VkDeviceSize batch_vertices = 0;
        VkDeviceSize batch_indices = 0;

        try
        {
            // ── Geometry ranges ───────────────────────────────────────
            // Allocated before anything is recorded: a full pool rejects
            // the batch with only the ranges of this batch to give back.
            for (const CoreTypes::MeshData* mesh_data : _batch.meshes)
            {
                Mesh_GPU mesh;
                mesh.geometry = geometry_pool.Allocate(static_cast<uint32_t>(mesh_data->vertices.size()),
                                                       static_cast<uint32_t>(mesh_data->indices.size()));

                // From the full-precision positions, before packing.
                Mesh_GPU::Compute_bounding_sphere(*mesh_data, mesh.bounds_center, mesh.bounds_radius);

                meshes.push_back(mesh);

                batch_vertices += mesh_data->vertices.size();
                batch_indices += mesh_data->indices.size();
            }

            // ── Staging buffer: vertices | indices | mesh table entries ──
            // One buffer for the whole batch. Each section starts on a
            // 16-byte boundary; the copies below take one region per mesh.
            std::vector<VkBufferCopy> vertex_copies;
            std::vector<VkBufferCopy> index_copies;
            VkBufferCopy              table_copy{};

            if (mesh_count > 0)
            {
                const VkDeviceSize vertex_section_size = batch_vertices * sizeof(Geometry_Pool::Vertex);
                const VkDeviceSize index_section = Align_up(vertex_section_size, 16);
                const VkDeviceSize table_section = Align_up(index_section + batch_indices * sizeof(Geometry_Pool::Index), 16);
                const VkDeviceSize staging_size = table_section + mesh_count * sizeof(Mesh_Info_GPU);

                // Host-coherent and persistently mapped (Cpu_To_Gpu): the
                // writes below need no flush before the submit.
                staging = Vulkan_Buffer_Utils::Create_buffer(allocator.Get_handle(), staging_size,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);

                uint8_t* const       staging_bytes = static_cast<uint8_t*>(staging.mapped_ptr);
                Mesh_Info_GPU* const table_entries = reinterpret_cast<Mesh_Info_GPU*>(staging_bytes + table_section);

                VkDeviceSize vertex_cursor = 0;
                VkDeviceSize index_cursor = index_section;

                vertex_copies.reserve(mesh_count);
                index_copies.reserve(mesh_count);

                for (size_t i = 0; i < mesh_count; ++i)
                {
                    const CoreTypes::MeshData& mesh_data = *_batch.meshes[i];
                    const Mesh_GPU&            mesh = meshes[first_mesh + i];

                    // Vertices, packed into the layout the pool (and
                    // mesh.vert) reads.
                    Geometry_Pool::Vertex* const packed = reinterpret_cast<Geometry_Pool::Vertex*>(staging_bytes + vertex_cursor);

                    for (size_t v = 0; v < mesh_data.vertices.size(); ++v)
                        packed[v] = CoreTypes::Vertex_Packing::Pack(mesh_data.vertices[v]);

                    const VkDeviceSize vertex_size = mesh_data.vertices.size() * sizeof(Geometry_Pool::Vertex);
                    vertex_copies.push_back({ vertex_cursor, Geometry_Pool::Vertex_byte_offset(mesh.geometry), vertex_size });
                    vertex_cursor += vertex_size;

                    // Indices: MeshData keeps them as uint32_t whatever its
                    // index_type says, and the pool has a single index type,
                    // VK_INDEX_TYPE_UINT32, so they are copied unchanged.
                    // They stay local to the mesh: vertexOffset rebases them.
                    const VkDeviceSize index_size = mesh_data.indices.size() * sizeof(Geometry_Pool::Index);
                    std::memcpy(staging_bytes + index_cursor, mesh_data.indices.data(), static_cast<size_t>(index_size));

                    index_copies.push_back({ index_cursor, Geometry_Pool::Index_byte_offset(mesh.geometry), index_size });
                    index_cursor += index_size;

                    // Mesh table entry, the GPU copy of the record.
                    Mesh_Info_GPU& entry = table_entries[i];
                    entry.bounding_sphere = MathLib::Vector4(mesh.bounds_center, mesh.bounds_radius);
                    entry.first_index = mesh.geometry.first_index;
                    entry.index_count = mesh.geometry.index_count;
                    entry.vertex_offset = static_cast<int32_t>(mesh.geometry.first_vertex);
                    entry.vertex_count = mesh.geometry.vertex_count;
                }

                // The gpu ids of one batch are consecutive, so their table
                // entries are one contiguous region.
                table_copy.srcOffset = table_section;
                table_copy.dstOffset = static_cast<VkDeviceSize>(first_mesh) * sizeof(Mesh_Info_GPU);
                table_copy.size = static_cast<VkDeviceSize>(mesh_count) * sizeof(Mesh_Info_GPU);
            }

            // ── Frames in flight ──────────────────────────────────────
            // They read the geometry pool and the mesh table while this
            // batch writes them. The ranges written are new, so no frame
            // reads them, but each buffer is a single resource: an indexed
            // or indirect draw is modeled by validation tools as reading the
            // whole bound vertex buffer, and a descriptor as reading its
            // whole range. Waiting for the frames in flight first orders the
            // copies after every earlier read without relying on the ranges
            // being disjoint. Uploads are synchronous anyway, and the copies
            // would queue behind those frames.
            if (mesh_count > 0)
                Wait_for_frames_in_flight();

            // ── One command buffer for the whole batch ────────────────
            transfer_cmd = transfer_command_pool.Allocate_primary();

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(transfer_cmd, &begin_info), "Upload_batch: begin transfer command buffer");

            if (mesh_count > 0)
            {
                // Three copies for the whole batch, one region per mesh.
                // The ranges were just allocated, so no frame in flight
                // reads them: no barrier is needed before the copies.
                vkCmdCopyBuffer(transfer_cmd, staging.buffer, geometry_pool.Get_vertex_buffer(),
                    static_cast<uint32_t>(vertex_copies.size()), vertex_copies.data());
                vkCmdCopyBuffer(transfer_cmd, staging.buffer, geometry_pool.Get_index_buffer(),
                    static_cast<uint32_t>(index_copies.size()), index_copies.data());
                vkCmdCopyBuffer(transfer_cmd, staging.buffer, mesh_table_buffer.buffer, 1, &table_copy);

                // A barrier's second scope reaches every later submission
                // of the queue, so this makes the copies visible to all the
                // frames that will read them: vertex and index fetch of the
                // draws, the vertex shader of the bounds view and the
                // culling pass (mesh table).
                Vulkan_Buffer_Utils::Record_memory_barrier(transfer_cmd,
                    { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT },
                    { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT });
            }

            // ── Textures ───────────────────────────────────────────────
            // No barriers are needed BETWEEN assets: each Texture_GPU
            // barriers its own image, so the recordings touch disjoint
            // resources. The barriers that do exist (layout transitions,
            // mip generation) are internal to each Texture_GPU.
            for (const Texture_Upload& upload : _batch.textures)
            {
                textures.emplace_back(device, allocator.Get_handle(), transfer_cmd, *upload.data, upload.format);
            }

            // ── End, submit, wait: ONCE for the whole batch ──────────
            Submit_and_wait_transfer(transfer_cmd);
        }
        catch (...)
        {
            // Nothing of this batch reached the GPU in a usable state: give
            // back the ranges and drop the registry entries added above
            // (Texture_GPU destructors free their images; the transfer
            // either never ran or was waited for by Submit_and_wait_transfer
            // before it threw), then the staging buffer and the command
            // buffer.
            vkDeviceWaitIdle(device.Get_logical_device_handle());

            for (size_t i = first_mesh; i < meshes.size(); ++i)
                geometry_pool.Free(meshes[i].geometry);

            meshes.erase(meshes.begin() + static_cast<std::ptrdiff_t>(first_mesh), meshes.end());
            textures.erase(textures.begin() + static_cast<std::ptrdiff_t>(first_texture), textures.end());
            Vulkan_Buffer_Utils::Destroy_buffer(allocator.Get_handle(), staging);

            if (transfer_cmd != VK_NULL_HANDLE)
                transfer_command_pool.Free(transfer_cmd);

            throw;
        }

        transfer_command_pool.Free(transfer_cmd);

        // ── Post-upload ───────────────────────────────────────────
        // The fence is signaled, so the GPU has consumed every staging
        // buffer in the batch and they can all be freed now.
        Vulkan_Buffer_Utils::Destroy_buffer(allocator.Get_handle(), staging);

        for (size_t i = first_mesh; i < meshes.size(); ++i)
            result.mesh_gpu_ids.push_back(static_cast<uint32_t>(i));

        for (size_t i = first_texture; i < textures.size(); ++i)
        {
            textures[i].Release_staging_buffers();

            // Register in the global bindless texture array. Only the view
            // is registered: the sampler is chosen per draw, from the
            // sampler array, by the material's preset. The bindless index,
            // not the registry index i, is what callers store and what
            // shaders use.
            //
            // Cannot throw, which keeps the strong guarantee although it
            // runs after the submit: the free slots were checked before
            // recording, the registry reserves storage for every slot at
            // construction, the view of a constructed Texture_GPU is never
            // null, and result.texture_bindless_indices was reserved above.
            result.texture_bindless_indices.push_back( bindless_registry.Register_texture(textures[i].Get_image_view()));
        }

        std::cout << "[Renderer] Batch uploaded: "
            << mesh_count << " mesh(es) (" << batch_vertices << " vertices, " << batch_indices << " indices into the geometry pool), "
            << texture_count << " texture(s) - 1 command buffer, 1 submit.\n";

        return result;
    }

    uint32_t Renderer::Upload_mesh(const CoreTypes::MeshData& _mesh_data)
    {
        Upload_Batch batch;
        batch.meshes.push_back(&_mesh_data);

        const Upload_Batch_Result result = Upload_batch(batch);

        if (result.mesh_gpu_ids.size() != 1)
            throw std::logic_error("Upload_mesh: single-mesh batch returned the wrong number of ids");

        return result.mesh_gpu_ids[0];
    }

    uint32_t Renderer::Upload_texture(const CoreTypes::ImageData& _image_data, VkFormat _format)
    {
        Upload_Batch batch;
        batch.textures.push_back({ &_image_data, _format });

        const Upload_Batch_Result result = Upload_batch(batch);

        if (result.texture_bindless_indices.size() != 1)
            throw std::logic_error("Upload_texture: single-texture batch returned the wrong number of indices");

        return result.texture_bindless_indices[0];
    }

    uint32_t Renderer::Upload_texture(const CoreTypes::ImageData& _image_data)
    {
        return Upload_texture(_image_data, Vulkan_Image_Utils::To_vk_format(_image_data.format));
    }
    void Renderer::Release_mesh(uint32_t _gpu_id)
    {
        if (_gpu_id >= meshes.size() || meshes[_gpu_id].released)
            return;

        // The unit sphere of the bounds view is internal: releasing it
        // would leave that view drawing a freed range.
        if (_gpu_id == bounds_sphere_mesh_id)
        {
            std::cerr << "[Renderer] Release_mesh: mesh " << _gpu_id << " is internal to the Renderer and is not released.\n";
            return;
        }

        // Frames recorded from now on skip it. Every frame submitted so far
        // may still draw it, so its range waits for the last of them.
        meshes[_gpu_id].released = true;
        retired_meshes.push_back({ _gpu_id, submitted_frames });

        // Freed at once when every submitted frame has already completed.
        Free_retired_meshes();
    }

    void Renderer::Wait_for_frames_in_flight()
    {
        if (frames.empty())
            return;

        std::array<VkFence, FRAMES_IN_FLIGHT> fences{};

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
            fences[i] = frames[i].in_flight_fence;

        VK_CHECK(vkWaitForFences(device.Get_logical_device_handle(), FRAMES_IN_FLIGHT, fences.data(), VK_TRUE, UINT64_MAX),
            "Wait_for_frames_in_flight: wait for the frame fences");

        // Every submitted frame has completed: released geometry can go.
        completed_frames = submitted_frames;
        Free_retired_meshes();
    }

    void Renderer::Free_retired_meshes()
    {
        for (size_t i = 0; i < retired_meshes.size(); )
        {
            if (retired_meshes[i].last_frame_serial <= completed_frames)
            {
                geometry_pool.Free(meshes[retired_meshes[i].gpu_id].geometry);

                retired_meshes[i] = retired_meshes.back();
                retired_meshes.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }

    void Renderer::Upload_default_textures()
    {
        namespace Default = CoreTypes::Default_Texture;

        // One opaque texel. Mip generation is skipped for 1x1 images
        // (Compute_mip_levels(1, 1) == 1).
        const auto make_texel = [](uint8_t _r, uint8_t _g, uint8_t _b, CoreTypes::Pixel_Format _format)
            {
                CoreTypes::ImageData image;
                image.pixels = { _r, _g, _b, 255 };
                image.width = 1;
                image.height = 1;
                image.mip_levels = 1;
                image.format = _format;
                return image;
            };

        // Indexed by slot, so each texel stays tied to its constant even
        // if the Default_Texture values are ever reordered.
        std::array<CoreTypes::ImageData, Default::Count> defaults;
        defaults[Default::Error] = make_texel(255, 0, 255, CoreTypes::Pixel_Format::RGBA8_SRGB);
        defaults[Default::White] = make_texel(255, 255, 255, CoreTypes::Pixel_Format::RGBA8_SRGB);
        defaults[Default::Black] = make_texel(0, 0, 0, CoreTypes::Pixel_Format::RGBA8_SRGB);
        // A normal is data, not color: UNORM, so 128 stays 0.5 when sampled.
        defaults[Default::Flat_Normal] = make_texel(128, 128, 255, CoreTypes::Pixel_Format::RGBA8_UNORM);

        Upload_Batch batch;
        for (const CoreTypes::ImageData& image : defaults)
            batch.textures.push_back({ &image, Vulkan_Image_Utils::To_vk_format(image.format) });

        const Upload_Batch_Result result = Upload_batch(batch);

        // The slots are a contract with every Draw_Item. A mismatch means
        // something was registered before this call.
        bool in_place = result.texture_bindless_indices.size() == Default::Count;
        for (uint32_t i = 0; in_place && i < Default::Count; ++i)
            in_place = result.texture_bindless_indices[i] == i;

        if (!in_place)
            throw std::logic_error("Renderer: the default textures did not land in bindless slots 0.."
                + std::to_string(Default::Count - 1) + "; something was registered before them");

        std::cout << "[Renderer] Default textures in bindless slots 0-" << (Default::Count - 1)
                  << " (Error, White, Black, Flat_Normal).\n";
    }

    // =========================================================
    // Init_procedural_pass
    // =========================================================

    void Renderer::Init_procedural_pass()
    {
        // Fixed size: independent of the swapchain, so the image is never
        // recreated on resize and its bindless slot never changes.
        constexpr uint32_t PROCEDURAL_TEXTURE_SIZE = 256;

        // Storage image support for R8G8B8A8_UNORM with optimal tiling is
        // mandatory in Vulkan. Must match the rgba8 qualifier of
        // procedural.comp. SRGB formats rarely support storage.
        constexpr VkFormat PROCEDURAL_TEXTURE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

        VkDevice dev = device.Get_logical_device_handle();

        // ── Image and initial clear ────────────────────────────────
        // The constructor records the clear that leaves every texel at zero
        // in SHADER_READ_ONLY_OPTIMAL. It is submitted and waited for here,
        // before the first frame, so the slot is valid from its first read.
        VkCommandBuffer transfer_cmd = transfer_command_pool.Allocate_primary();

        try
        {
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(transfer_cmd, &begin_info), "Init_procedural_pass: begin transfer command buffer");

            procedural_image.emplace(device, allocator.Get_handle(), transfer_cmd,
                PROCEDURAL_TEXTURE_SIZE, PROCEDURAL_TEXTURE_SIZE, PROCEDURAL_TEXTURE_FORMAT);

            Submit_and_wait_transfer(transfer_cmd);
        }
        catch (...)
        {
            // Same recovery as Upload_batch: the clear either never ran or
            // was waited for, so the image can be destroyed right away.
            vkDeviceWaitIdle(dev);
            procedural_image.reset();
            transfer_command_pool.Free(transfer_cmd);
            throw;
        }

        transfer_command_pool.Free(transfer_cmd);

        // ── Bindless slot (set 3) ──────────────────────────────────
        // Read by the draws as a sampled image. The declared layout of the
        // slot (SHADER_READ_ONLY_OPTIMAL) holds after the initial clear and
        // after every Storage_Image::End_write.
        procedural_texture_index = bindless_registry.Register_texture(procedural_image->Get_image_view());

        // ── Set 1: storage image descriptor ────────────────────────
        // Set 1 of the compute contract (Binding_Per_Pass).
        const VkDescriptorSetLayout per_pass_layout = descriptor_layouts.Get(Descriptor_Set::Per_Pass, Pipeline_Kind::Compute);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &per_pass_layout;

        VK_CHECK(vkAllocateDescriptorSets(dev, &alloc_info, &per_pass_set),
            "Init_procedural_pass: failed to allocate the set 1 descriptor set");

        // GENERAL: the layout the image is in while the dispatch writes it
        // (between Begin_write and End_write), not the layout it rests in.
        VkDescriptorImageInfo image_info{};
        image_info.sampler = VK_NULL_HANDLE;
        image_info.imageView = procedural_image->Get_image_view();
        image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = per_pass_set;
        write.dstBinding = Binding_Per_Pass::Procedural_Output;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;

        // Written once, before any frame is recorded: the only moment the
        // set can be updated without racing a frame in flight.
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

        // The image holds the zeros of its initial clear: the first frame
        // generates its content.
        procedural_dirty = true;

        std::cout << "[Renderer] Procedural texture " << PROCEDURAL_TEXTURE_SIZE << "x" << PROCEDURAL_TEXTURE_SIZE
            << " (" << Vulkan_Utils::Vk_format_to_string(PROCEDURAL_TEXTURE_FORMAT)
            << ") in bindless slot " << procedural_texture_index << ", generated when its content changes.\n";
    }

    // =========================================================
    // Material and mesh tables
    // =========================================================

    void Renderer::Init_global_tables()
    {
        VkDevice dev = device.Get_logical_device_handle();

        // ── Buffers ────────────────────────────────────────────────
        // Material table: host-visible and coherent (Cpu_To_Gpu),
        // persistently mapped: Register_material writes each new slot in
        // place.
        material_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator.Get_handle(), sizeof(Material_GPU) * MAX_MATERIALS,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);

        // Mesh table: device-local, written by the transfer of the upload
        // that creates each mesh (Upload_batch), never by the CPU directly.
        mesh_table_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator.Get_handle(), sizeof(Mesh_Info_GPU) * MAX_MESHES,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only);

        // Reserved up front: Register_material then never reallocates, so
        // its push_back cannot throw after the capacity check.
        registered_materials.reserve(MAX_MATERIALS);

        // ── Set 2: material table descriptor ───────────────────────
        // Set 2 is shared by both contracts.
        const VkDescriptorSetLayout per_material_layout = descriptor_layouts.Get(Descriptor_Set::Per_Material, Pipeline_Kind::Graphics);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &per_material_layout;

        VK_CHECK(vkAllocateDescriptorSets(dev, &alloc_info, &per_material_set),
            "Init_global_tables: failed to allocate the set 2 descriptor set");

        // Both ranges are the whole CAPACITY: the descriptors are written
        // once and entries are added in place afterwards.
        VkDescriptorBufferInfo material_info{};
        material_info.buffer = material_buffer.buffer;
        material_info.offset = 0;
        material_info.range = sizeof(Material_GPU) * MAX_MATERIALS;

        VkDescriptorBufferInfo mesh_info{};
        mesh_info.buffer = mesh_table_buffer.buffer;
        mesh_info.offset = 0;
        mesh_info.range = sizeof(Mesh_Info_GPU) * MAX_MESHES;

        std::array<VkWriteDescriptorSet, 2> writes{};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = per_material_set;
        writes[0].dstBinding = Binding_Per_Material::Materials;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &material_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = per_material_set;
        writes[1].dstBinding = Binding_Per_Material::Meshes;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &mesh_info;

        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // ── Default material ───────────────────────────────────────
        // A default-constructed Material_Desc is the default material.
        // Slot 0 is a contract with every Draw_Item: a mismatch means
        // something was registered before this call.
        const uint32_t default_slot = Register_material(Material_Desc{});

        if (default_slot != CoreTypes::Default_Material)
            throw std::logic_error("Renderer: the default material did not land in slot "
                + std::to_string(CoreTypes::Default_Material) + "; something was registered before it");

        std::cout << "[Renderer] Material table: " << MAX_MATERIALS << " slots, default material in slot "
            << CoreTypes::Default_Material << ". Mesh table: " << MAX_MESHES << " entries.\n";
    }

    // =========================================================
    // Clustered lighting resources
    // =========================================================

    void Renderer::Init_light_clusters()
    {
        assert(per_pass_set != VK_NULL_HANDLE && "Init_light_clusters: Init_procedural_pass() allocates per_pass_set and must run first");

        // Device-local: written by vkCmdUpdateBuffer inside the frames
        // (TRANSFER_DST), read by cluster_lights.comp (STORAGE_BUFFER).
        cluster_aabb_buffer = Vulkan_Buffer_Utils::Create_buffer(allocator.Get_handle(), sizeof(Cluster_AABB_GPU) * CLUSTER_COUNT,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Vulkan_Buffer_Utils::Buffer_Access::Gpu_Only);

        cluster_aabb_scratch.resize(CLUSTER_COUNT);

        // Built by the first frame: nothing reads the buffer before it.
        cluster_aabbs_valid = false;

        VkDescriptorBufferInfo aabb_info{};
        aabb_info.buffer = cluster_aabb_buffer.buffer;
        aabb_info.offset = 0;
        aabb_info.range = sizeof(Cluster_AABB_GPU) * CLUSTER_COUNT;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = per_pass_set;
        write.dstBinding = Binding_Per_Pass::Cluster_AABBs;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &aabb_info;

        // Written once, before any frame is recorded.
        vkUpdateDescriptorSets(device.Get_logical_device_handle(), 1, &write, 0, nullptr);

        std::cout << "[Renderer] Clustered lighting: " << CLUSTER_TILES_X << "x" << CLUSTER_TILES_Y << "x" << CLUSTER_SLICES
            << " clusters, slices up to " << CLUSTER_MAX_DISTANCE << " units, " << CLUSTER_LIGHT_INDEX_CAPACITY
            << " light index entries per frame, " << MAX_LIGHTS << " lights max.\n";
    }

    // =========================================================
    // Debug resources
    // =========================================================

    void Renderer::Init_debug_meshes()
    {
        const CoreTypes::MeshData unit_sphere = Build_unit_sphere(BOUNDS_SPHERE_SEGMENTS, BOUNDS_SPHERE_RINGS);

        bounds_sphere_mesh_id = Upload_mesh(unit_sphere);

        std::cout << "[Renderer] Bounding volume view: unit sphere uploaded as mesh " << bounds_sphere_mesh_id
            << (device.Is_fill_mode_non_solid_enabled() ? " (wireframe).\n" : " (filled, blended: fillModeNonSolid unavailable).\n");
    }

    void Renderer::Name_debug_objects()
    {
        if (!debug_utils.Is_enabled())
            return;

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            const Frame_Data&  frame = frames[i];
            const std::string suffix = "_Frame" + std::to_string(i);

            const auto name_buffer = [&](const Vulkan_Buffer_Utils::Buffer_Allocation& _buffer, const char* _name)
                {
                    debug_utils.Set_name(_buffer.buffer, VK_OBJECT_TYPE_BUFFER, (std::string(_name) + suffix).c_str());
                };

            name_buffer(frame.uniform_buffer, "Frame_UBO");
            name_buffer(frame.light_buffer, "Lights");
            name_buffer(frame.object_buffer, "Objects");
            name_buffer(frame.cpu_draw_command_buffer, "Cpu_Draw_Commands");
            name_buffer(frame.cluster_grid_buffer, "Cluster_Grid");
            name_buffer(frame.cluster_light_index_buffer, "Cluster_Light_Indices");
            name_buffer(frame.cluster_counter_buffer, "Cluster_Counters");
            name_buffer(frame.gpu_draw_command_buffer, "Gpu_Draw_Commands");
            name_buffer(frame.gpu_draw_count_buffer, "Gpu_Draw_Count");
            name_buffer(frame.stats_readback_buffer, "Stats_Readback");

            debug_utils.Set_name(frame.Get_command_buffer(), VK_OBJECT_TYPE_COMMAND_BUFFER, ("Frame_Commands" + suffix).c_str());
            debug_utils.Set_name(descriptor_sets[i], VK_OBJECT_TYPE_DESCRIPTOR_SET, ("Per_Frame_Set" + suffix).c_str());
        }

        debug_utils.Set_name(geometry_pool.Get_vertex_buffer(), VK_OBJECT_TYPE_BUFFER, "Geometry_Pool_Vertices");
        debug_utils.Set_name(geometry_pool.Get_index_buffer(), VK_OBJECT_TYPE_BUFFER, "Geometry_Pool_Indices");
        debug_utils.Set_name(material_buffer.buffer, VK_OBJECT_TYPE_BUFFER, "Material_Table");
        debug_utils.Set_name(mesh_table_buffer.buffer, VK_OBJECT_TYPE_BUFFER, "Mesh_Table");
        debug_utils.Set_name(cluster_aabb_buffer.buffer, VK_OBJECT_TYPE_BUFFER, "Cluster_AABBs");

        debug_utils.Set_name(per_pass_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Per_Pass_Set");
        debug_utils.Set_name(composite_input_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Composite_Input_Set");
        debug_utils.Set_name(per_material_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Per_Material_Set");

        debug_utils.Set_name(procedural_pipeline.Get_handle(), VK_OBJECT_TYPE_PIPELINE, "Procedural_Compute");
        debug_utils.Set_name(cluster_pipeline.Get_handle(), VK_OBJECT_TYPE_PIPELINE, "Cluster_Lights_Compute");
        debug_utils.Set_name(cull_pipeline.Get_handle(), VK_OBJECT_TYPE_PIPELINE, "Cull_Objects_Compute");
        debug_utils.Set_name(pipeline_registry.Get_by_id(opaque_pipeline_id), VK_OBJECT_TYPE_PIPELINE, "Mesh_Opaque");
        debug_utils.Set_name(pipeline_registry.Get_by_id(transparent_pipeline_id), VK_OBJECT_TYPE_PIPELINE, "Mesh_Transparent");
        debug_utils.Set_name(pipeline_registry.Get_by_id(bounds_pipeline_id), VK_OBJECT_TYPE_PIPELINE, "Bounds_Debug");
        debug_utils.Set_name(pipeline_registry.Get_by_id(composite_pipeline_id), VK_OBJECT_TYPE_PIPELINE, "Oit_Composite");

        if (procedural_image.has_value())
            debug_utils.Set_name(procedural_image->Get_image(), VK_OBJECT_TYPE_IMAGE, "Procedural_Texture");

        if (gpu_timer.Is_supported())
            debug_utils.Set_name(gpu_timer.Get_query_pool(), VK_OBJECT_TYPE_QUERY_POOL, "Gpu_Timer_Queries");

        Name_oit_targets();
    }

    void Renderer::Name_oit_targets()
    {
        if (!debug_utils.Is_enabled())
            return;

        debug_utils.Set_name(oit_resources.Get_accumulation_image(), VK_OBJECT_TYPE_IMAGE, "Oit_Accumulation");
        debug_utils.Set_name(oit_resources.Get_revealage_image(), VK_OBJECT_TYPE_IMAGE, "Oit_Revealage");
    }

    uint32_t Renderer::Register_material(const Material_Desc& _desc)
    {
        assert(material_buffer.mapped_ptr != nullptr && "Register_material called before Init_material_table");

        // Validated once here instead of once per draw per frame. A slot
        // that passes these checks stays valid because slots are never
        // rewritten, provided the texture is not released through
        // Bindless_Registry::Release_texture while a material references
        // it (nothing releases textures today).
        if (!bindless_registry.Is_texture_registered(_desc.albedo_texture_index))
            throw std::invalid_argument("Register_material: albedo_texture_index " + std::to_string(_desc.albedo_texture_index)
                + " is not a registered bindless texture slot");

        const uint32_t sampler_index = static_cast<uint32_t>(_desc.sampler);

        if (sampler_index >= static_cast<uint32_t>(CoreTypes::Sampler_Preset::Count))
            throw std::invalid_argument("Register_material: sampler " + std::to_string(sampler_index)
                + " is not a Sampler_Preset value");

        // Deduplication by value. Linear: runs at registration time only,
        // over at most MAX_MATERIALS entries.
        for (uint32_t slot = 0; slot < static_cast<uint32_t>(registered_materials.size()); ++slot)
        {
            if (registered_materials[slot] == _desc)
                return slot;
        }

        if (registered_materials.size() >= MAX_MATERIALS)
            throw std::runtime_error("Register_material: the material table is full (" + std::to_string(MAX_MATERIALS)
                + " slots): raise MAX_MATERIALS in Frame_Data.hpp");

        const uint32_t slot = static_cast<uint32_t>(registered_materials.size());
        registered_materials.push_back(_desc);

        // Written in place: this slot was never referenced by a submitted
        // command, and host-coherent writes are visible to the next submit
        // without a flush or a barrier.
        Material_GPU& gpu_material = static_cast<Material_GPU*>(material_buffer.mapped_ptr)[slot];
        gpu_material.base_color = _desc.base_color;
        gpu_material.albedo_texture_index = _desc.albedo_texture_index;
        gpu_material.albedo_sampler_index = sampler_index;
        gpu_material._padding0 = 0;
        gpu_material._padding1 = 0;

        return slot;
    }

    // =========================================================
    // Surface size
    // =========================================================

    void Renderer::Notify_framebuffer_resized()
    {
        framebuffer_resized = true;
    }

    bool Renderer::Recreate_swapchain_if_needed()
    {
        if (!framebuffer_resized) return true;

        // A minimized window has no framebuffer to build a swapchain for.
        // The flag stays set; the next call after restoring handles it.
        if (window.Is_minimized()) return false;

        Recreate_swapchain();
        return true;
    }

    void Renderer::Get_render_size(uint32_t& _out_width, uint32_t& _out_height) const
    {
        const VkExtent2D extent = swapchain.Get_extent();
        _out_width = extent.width;
        _out_height = extent.height;
    }

    // =========================================================
    // Render
    // =========================================================

    void Renderer::Render(const CoreTypes::RenderPacket& _packet)
    {
        VkDevice dev = device.Get_logical_device_handle();

        // A resize reported by the window is applied before the frame, so
        // the frame is rendered at the new size. While minimized there is
        // nothing to render into.
        if (framebuffer_resized && !Recreate_swapchain_if_needed()) return;

        Frame_Data& frame = frames[current_frame];

        // ── Wait for this frame slot to be free ───────────────────
        // A device loss surfaces here as an exception instead of the loop
        // submitting forever to a dead device.
        VK_CHECK(vkWaitForFences(dev, 1, &frame.in_flight_fence, VK_TRUE, UINT64_MAX),
            "Render: wait for frame fence");

        // ── Work of the previous use of this slot ─────────────────
        // Its fence is signaled: that frame and every earlier submission
        // have completed (a fence signal includes all the work submitted
        // before it). Geometry released before then can go back to the
        // pool, and the GPU results of that frame can be read.
        completed_frames = std::max(completed_frames, slot_frame_serial[current_frame]);
        Free_retired_meshes();
        Read_frame_statistics(current_frame);

        // ── Acquire swapchain image ───────────────────────────────
        uint32_t image_index = 0;
        const VkResult acquire_result = vkAcquireNextImageKHR(
            dev,
            swapchain.Get_handle(),
            UINT64_MAX,
            frame.image_available_semaphore,
            VK_NULL_HANDLE,
            &image_index
        );

        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            Recreate_swapchain();
            return;  // skip this frame, retry next
        }

        // VK_SUBOPTIMAL_KHR is a success: the image was acquired and the
        // semaphore will be signaled, so the frame must be rendered and
        // presented; the swapchain is recreated after the present.
        VK_CHECK(acquire_result, "Render: failed to acquire swapchain image");

        Image_Sync& sync = image_sync[image_index];

        // ── Wait for this image's previous present to complete ────
        // With swapchain_maintenance1 the present operation signals
        // present_fence when done. Waiting on it before reusing
        // render_finished (which that present waits on) is what makes
        // re-signaling the semaphore legal. present_pending is only ever
        // true when the fence exists.
        if (sync.present_pending)
        {
            VK_CHECK(vkWaitForFences(dev, 1, &sync.present_fence, VK_TRUE, UINT64_MAX),
                "Render: wait for present fence");
            VK_CHECK(vkResetFences(dev, 1, &sync.present_fence), "Render: reset present fence");
            sync.present_pending = false;
        }

        // ── Update per-frame buffers ──────────────────────────────
        Update_culling_frustum(_packet);
        Write_frame_uniforms(frame, _packet);

        // ── Record commands ───────────────────────────────────────
        frame.command_pool.Reset_command_buffer(0);
        Record_command_buffer(frame, _packet, image_index);

        // ── Reset fence just before submit (not before recording) ─
        // Only a submit signals the fence again. Were it reset before
        // recording, an exception thrown while recording would leave it
        // unsignaled with no submit pending, and the next
        // vkWaitForFences(UINT64_MAX) on this frame slot would never
        // return.
        VK_CHECK(vkResetFences(dev, 1, &frame.in_flight_fence), "Render: reset frame fence");

        // ── Submit ────────────────────────────────────────────────
        const VkPipelineStageFlags wait_stages[] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        };

        const VkCommandBuffer command_buffer = frame.Get_command_buffer();

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &frame.image_available_semaphore;
        submit_info.pWaitDstStageMask = wait_stages;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &sync.render_finished;

        VK_CHECK(vkQueueSubmit(device.Get_graphics_queue(), 1, &submit_info, frame.in_flight_fence),
            "Render: failed to submit command buffer");

        // The frame is in flight from here on: its serial is what released
        // geometry waits for, and what its record is read back against.
        slot_frame_serial[current_frame] = ++submitted_frames;
        frame_records[current_frame].recorded = true;

        // The procedural image this submission writes stays valid for every
        // later frame (End_write makes it visible to all later work on the
        // queue). Cleared only here, after a successful submit: a recording
        // that threw or a failed submit leaves the flag set, and the next
        // frame records the pass again.
        if (frame_records[current_frame].procedural_recorded)
            procedural_dirty = false;

        // ── Present ───────────────────────────────────────────────
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &sync.render_finished;
        present_info.swapchainCount = 1;
        VkSwapchainKHR swapchain_handle = swapchain.Get_handle();
        present_info.pSwapchains = &swapchain_handle;
        present_info.pImageIndices = &image_index;

        // Attach a present fence so the completion of this present is
        // known the next time this image comes around
        // (VK_KHR_swapchain_maintenance1; the instance and device halves
        // were both negotiated before the fence was created).
        VkSwapchainPresentFenceInfoKHR present_fence_info{};
        if (sync.present_fence != VK_NULL_HANDLE)
        {
            present_fence_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
            present_fence_info.swapchainCount = 1;
            present_fence_info.pFences = &sync.present_fence;
            present_info.pNext = &present_fence_info;

            sync.present_pending = true;
        }

        const VkResult present_result = vkQueuePresentKHR(device.Get_present_queue(), &present_info);

        // OUT_OF_DATE from present and SUBOPTIMAL from either call mean
        // the swapchain no longer matches the surface. The semaphore wait
        // of a rejected present is still executed by the queue, so the
        // retirement logic in Recreate_swapchain applies as usual.
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR ||
            present_result == VK_SUBOPTIMAL_KHR ||
            acquire_result == VK_SUBOPTIMAL_KHR)
        {
            Recreate_swapchain();
        }
        else
        {
            VK_CHECK(present_result, "Render: failed to present");
        }

        Flush_retired_sync(false);

        current_frame = (current_frame + 1) % FRAMES_IN_FLIGHT;
    }

    // =========================================================
    // Write_frame_uniforms
    // =========================================================

    void Renderer::Write_frame_uniforms(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet)
    {
        Frame_UBO ubo{};
        ubo.view = _packet.view.view;
        ubo.projection = _packet.view.projection;
        ubo.view_projection = _packet.view.view_projection;
        ubo.camera_position = _packet.view.camera_position;

        const uint32_t packet_lights = static_cast<uint32_t>(_packet.lights.size());

        // Reported when the overflow starts and whenever the light count
        // changes while it lasts, so a scene that stays over the limit
        // does not print on every frame.
        if (packet_lights > MAX_LIGHTS)
        {
            if (packet_lights != reported_light_overflow)
            {
                std::cerr << "[Renderer] " << packet_lights << " lights in the packet, only "
                    << MAX_LIGHTS << " are uploaded (directional lights first).\n";
                reported_light_overflow = packet_lights;
            }
        }
        else
        {
            reported_light_overflow = 0;
        }

        // Lights go to their own buffer, written straight into the mapped
        // memory: the destination already is a contiguous array of the
        // right type, so no intermediate array is needed.
        //
        // Directional lights first, then the point and spot lights, each
        // group in packet order. Directional lights reach every fragment
        // and are never clustered: mesh.frag loops over the first
        // directional_light_count entries, and the cluster pass only
        // distributes the rest. Filling them first also means that an
        // overflow drops local lights, never a sun.
        Light_GPU* gpu_lights = static_cast<Light_GPU*>(_frame.light_buffer.mapped_ptr);

        uint32_t light_count = 0;
        uint32_t directional_count = 0;

        const auto write_light = [&](const CoreTypes::GPU_Light& _src)
            {
                Light_GPU& dst = gpu_lights[light_count++];

                dst.position_or_direction = _src.position_or_direction;
                dst.intensity = _src.intensity;
                dst.color = _src.color;
                dst.range = _src.range;
                dst.spot_direction = _src.spot_direction;
                dst.inner_angle = _src.inner_angle;
                dst.outer_angle = _src.outer_angle;
                dst.type = static_cast<int32_t>(_src.type);
                dst._padding0 = 0.0f;
                dst._padding1 = 0.0f;
            };

        for (const CoreTypes::GPU_Light& light : _packet.lights)
        {
            if (light.type == 0 && light_count < MAX_LIGHTS)
            {
                write_light(light);
                ++directional_count;
            }
        }

        for (const CoreTypes::GPU_Light& light : _packet.lights)
        {
            if (light.type != 0 && light_count < MAX_LIGHTS)
                write_light(light);
        }

        ubo.light_count = static_cast<int32_t>(light_count);

        // Also the range the cluster pass distributes (its push constants).
        uploaded_light_count = light_count;
        uploaded_directional_light_count = directional_count;

        // ── Clustered lighting ────────────────────────────────────
        const Cluster_Grid::Slice_Mapping slices = Cluster_Grid::Make_slice_mapping(_packet.view.near_plane);
        const VkExtent2D                  extent = swapchain.Get_extent();

        ubo.cluster_tiles_x = CLUSTER_TILES_X;
        ubo.cluster_tiles_y = CLUSTER_TILES_Y;
        ubo.cluster_slices = CLUSTER_SLICES;
        ubo.directional_light_count = directional_count;

        ubo.render_width = static_cast<float>(extent.width);
        ubo.render_height = static_cast<float>(extent.height);
        ubo.cluster_slice_scale = slices.scale;
        ubo.cluster_slice_bias = slices.bias;

        ubo.light_culling_mode = static_cast<uint32_t>(debug_settings.light_culling);
        ubo.cluster_debug_view = static_cast<uint32_t>(debug_settings.cluster_view);
        ubo.heatmap_max_lights = debug_settings.heatmap_max_lights;
        ubo._padding0 = 0;

        // ── Culling ───────────────────────────────────────────────
        // The frustum of this frame, or the frozen one.
        for (uint32_t i = 0; i < FRUSTUM_PLANE_COUNT; ++i)
            ubo.frustum_planes[i] = culling_frustum.planes[i];

        std::memcpy(_frame.uniform_buffer.mapped_ptr, &ubo, sizeof(ubo));
    }

    void Renderer::Update_culling_frustum(const CoreTypes::RenderPacket& _packet)
    {
        // The first frame after the freeze was enabled provides the frozen
        // planes; later frames keep testing against them while the view
        // moves on.
        if (capture_frozen_frustum)
        {
            frozen_frustum = _packet.view.frustum;
            capture_frozen_frustum = false;
        }

        culling_frustum = debug_settings.freeze_culling ? frozen_frustum : _packet.view.frustum;
    }

    // =========================================================
    // Record_command_buffer
    // =========================================================

    void Renderer::Record_command_buffer(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet, uint32_t _image_index)
    {
        const VkCommandBuffer command_buffer = _frame.Get_command_buffer();

        // ── CPU side of the frame ─────────────────────────────────
        // The path that can draw this frame's opaque objects, then the
        // object buffer and the draw lists everything below consumes. The
        // transparent items are culled on the CPU exactly when the opaque
        // ones are culled on the GPU, with the same planes and the same
        // bounding volume test, so both passes discard the same objects.
        const Opaque_Draw_Path opaque_path = Resolve_opaque_path(debug_settings.opaque_path, _packet);
        const bool             gpu_draws = opaque_path == Opaque_Draw_Path::Gpu_Indirect || opaque_path == Opaque_Draw_Path::Gpu_Culled;
        const bool             frustum_culling = opaque_path == Opaque_Draw_Path::Gpu_Culled;

        Prepare_objects(_frame, _packet, frustum_culling);

        const uint32_t opaque_count = static_cast<uint32_t>(opaque_draws.size());

        if (opaque_path == Opaque_Draw_Path::Cpu_Indirect)
            Write_cpu_draw_commands(_frame);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info), "Record_command_buffer: begin");

        // Frame start point. Every block below is a Gpu_Scope: one name for
        // its debug label and its timed scope. The top-level scopes are all
        // opened outside the render pass, as the isolated timing mode
        // requires (it records a barrier before each of them).
        gpu_timer.Begin_frame(command_buffer, current_frame, debug_settings.isolate_gpu_timings);

        // ── Compute work: always before the render pass ───────────
        // Every compute pass that writes an image registered in the
        // bindless set is recorded here, with its two barriers
        // (Storage_Image::Begin_write / End_write), before
        // vkCmdBeginRenderPass:
        //   - a pipeline barrier inside the render pass requires a
        //     subpass self-dependency, and the render pass declares none
        //     (Vulkan_Render_Pass only declares dependencies between
        //     subpasses and from EXTERNAL);
        //   - End_write must execute before any draw that may read the
        //     bindless set (declared layout rule,
        //     Bindless_Registry::Register_texture).
        // The same holds for the buffers the compute passes write for the
        // draws (cluster lists, indirect commands): their barriers are
        // recorded here, outside the render pass.
        //
        // Compute passes bind their pipeline and descriptor sets at
        // VK_PIPELINE_BIND_POINT_COMPUTE: the sets bound below for
        // GRAPHICS are not visible to dispatches, and binding compute sets
        // does not disturb them. Every compute pipeline is built against
        // compute_pipeline_layout, so the four sets bound once here stay
        // bound across the pipeline changes of the passes below. Sets 0, 2
        // and 3 use the same handles as the graphics pass (same set layouts
        // in both pipeline layouts); set 1 is the compute one.
        const std::array<VkDescriptorSet, Descriptor_Set::Count> compute_sets = {
            descriptor_sets[current_frame], per_pass_set, per_material_set, bindless_registry.Get_set() };

        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline_layout.Get_handle(),
            Descriptor_Set::Per_Frame, static_cast<uint32_t>(compute_sets.size()), compute_sets.data(), 0, nullptr);

        // ── Procedural texture pass ───────────────────────────────
        // Writes every texel of procedural_image; the draws below sample it
        // through its bindless slot. Recorded only while its content is out
        // of date (procedural_dirty): once at startup today. Its barriers
        // serialize frames: Begin_write waits for the fragment and compute
        // work of everything submitted before, the previous frame included,
        // and End_write makes the compute passes below wait for the
        // dispatch. A frame without it lets the compute work of this frame
        // overlap the fragment work of the previous one.
        const bool record_procedural = procedural_dirty;

        constexpr uint32_t PROCEDURAL_GROUP_SIZE = 8;   // local_size_x / local_size_y of procedural.comp

        if (record_procedural)
        {
            assert(procedural_image.has_value() && "Record_command_buffer: Init_procedural_pass() has not run");

            const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Procedural texture", 0.9f, 0.6f, 0.1f);

            const VkExtent2D procedural_extent = procedural_image->Get_extent();

            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, procedural_pipeline.Get_handle());

            Procedural_Push_Constants procedural_push{};
            procedural_push.image_width = procedural_extent.width;
            procedural_push.image_height = procedural_extent.height;
            procedural_push.time = 0.0f;   // animated in milestone 1.3

            // Stage flags must match the range of compute_pipeline_layout
            // exactly (VK_SHADER_STAGE_COMPUTE_BIT).
            vkCmdPushConstants(command_buffer, compute_pipeline_layout.Get_handle(), VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(Procedural_Push_Constants), &procedural_push);

            // UNDEFINED -> GENERAL (contents discarded), after the reads of
            // the frames in flight that share this image (write-after-read).
            procedural_image->Begin_write(command_buffer);

            // Rounded up: a size that is not a multiple of the group size
            // still covers every texel; the shader discards the excess.
            const uint32_t group_count_x = (procedural_extent.width + PROCEDURAL_GROUP_SIZE - 1) / PROCEDURAL_GROUP_SIZE;
            const uint32_t group_count_y = (procedural_extent.height + PROCEDURAL_GROUP_SIZE - 1) / PROCEDURAL_GROUP_SIZE;
            vkCmdDispatch(command_buffer, group_count_x, group_count_y, 1);

            // GENERAL -> SHADER_READ_ONLY_OPTIMAL, compute writes made
            // visible to the fragment stage before the render pass begins,
            // in this frame and in every later one.
            procedural_image->End_write(command_buffer);
        }

        // ── Transfer: cluster boxes and counter resets ────────────
        // The boxes are rewritten only when the projection changed. The
        // atomic counters of both passes start every frame at zero; the
        // barrier after the fills also covers the box update, and reaches
        // every stage that reads what was reset: the atomics of the
        // compute passes, the indirect draw (draw count) and the
        // statistics copy. Its transfer write access also orders the
        // statistics copy of this frame after the copies earlier frames
        // made into the same readback buffer (write-after-write), with a
        // barrier instead of relying on the fence wait alone.
        {
            const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Uploads and resets", 0.5f, 0.5f, 0.5f);

            Record_cluster_aabb_update(command_buffer, _packet);

            Vulkan_Buffer_Utils::Record_zero_fill_and_barrier(command_buffer,
                { { _frame.cluster_counter_buffer.buffer, 0, sizeof(Cluster_Counters_GPU) },
                  { _frame.gpu_draw_count_buffer.buffer, 0, sizeof(Draw_Count_GPU) } },
                { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT });
        }

        // ── Light assignment to clusters ──────────────────────────
        {
            const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Light clusters", 1.0f, 0.9f, 0.2f);

            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cluster_pipeline.Get_handle());

            Cluster_Push_Constants cluster_push{};
            cluster_push.cluster_count = CLUSTER_COUNT;
            cluster_push.light_index_capacity = CLUSTER_LIGHT_INDEX_CAPACITY;
            cluster_push.first_local_light = uploaded_directional_light_count;
            cluster_push.light_count = uploaded_light_count;

            vkCmdPushConstants(command_buffer, compute_pipeline_layout.Get_handle(), VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(Cluster_Push_Constants), &cluster_push);

            // One invocation per cluster, rounded up to whole groups.
            vkCmdDispatch(command_buffer, (CLUSTER_COUNT + CLUSTER_GROUP_SIZE - 1) / CLUSTER_GROUP_SIZE, 1, 1);
        }

        // ── Frustum culling and draw generation ───────────────────
        // Only for the GPU paths; the opaque objects are entries
        // [0, opaque_count) of the object buffer. A frame that does not
        // record it simply has no culling scope.
        if (gpu_draws && opaque_count > 0)
        {
            const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Frustum culling", 0.2f, 0.8f, 1.0f);

            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline.Get_handle());

            Cull_Push_Constants cull_push{};
            cull_push.object_count = opaque_count;
            cull_push.command_capacity = MAX_OBJECTS;
            cull_push.pass_bit = CoreTypes::Render_Pass_Bit::Opaque;
            cull_push.frustum_culling = frustum_culling ? 1u : 0u;

            vkCmdPushConstants(command_buffer, compute_pipeline_layout.Get_handle(), VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(Cull_Push_Constants), &cull_push);

            vkCmdDispatch(command_buffer, (opaque_count + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE, 1, 1);
        }

        // ── Compute results -> consumers ──────────────────────────
        // One barrier for both passes: the cluster lists to the fragment
        // shader, the draw commands and their count to the indirect draw,
        // and the counters to the statistics copy after the render pass.
        // Forgetting the indirect part works "almost always", which is why
        // it is spelled out. Outside every scope: its cost shows as "other"
        // in the timings.
        Vulkan_Buffer_Utils::Record_compute_to_consumer_barrier(command_buffer,
            { VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT });

        // ── Render pass ───────────────────────────────────────────
        // Its scope holds the pass alone: the statistics copies, which only
        // depend on the barrier above, are recorded after it.
        uint8_t  bound_pipeline_id = 0xFF;
        uint32_t bind_count = 0;

        {
            const Gpu_Scope render_pass_scope(debug_utils, gpu_timer, command_buffer, "Render pass", 0.8f, 0.8f, 0.8f);

            // One clear value per attachment, in Render_Pass_Attachment
            // order.
            std::array<VkClearValue, Render_Pass_Attachment::Count> clear_values{};
            clear_values[Render_Pass_Attachment::Color].color =
                { { _packet.clear_color.r, _packet.clear_color.g, _packet.clear_color.b, _packet.clear_color.a } };

            // Reverse-Z: 0.0 is the far end, so that is what "nothing drawn yet"
            // means. Leave this at 1.0 and every fragment fails the GREATER test
            // - black screen, no validation error, nothing to debug.
            clear_values[Render_Pass_Attachment::Depth].depthStencil = { 0.0f, 0 };

            // Accumulation starts empty (no weighted color, no weight), and
            // revealage at 1 (all the background visible). Cleared when the
            // transparent subpass first uses them.
            clear_values[Render_Pass_Attachment::Oit_Accumulation].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
            clear_values[Render_Pass_Attachment::Oit_Revealage].color = { { 1.0f, 0.0f, 0.0f, 0.0f } };

            const VkExtent2D extent = swapchain.Get_extent();

            VkRenderPassBeginInfo render_pass_info{};
            render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            render_pass_info.renderPass = render_pass.Get_handle();
            render_pass_info.framebuffer = framebuffers.Get_framebuffer(_image_index);
            render_pass_info.renderArea.offset = { 0, 0 };
            render_pass_info.renderArea.extent = extent;
            render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
            render_pass_info.pClearValues = clear_values.data();

            vkCmdBeginRenderPass(command_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

            // ── Dynamic viewport + scissor ────────────────────────────
            // The same extent the packet's aspect ratio was derived from
            // (Get_render_size), so the projection and the viewport agree.
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(command_buffer, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.offset = { 0, 0 };
            scissor.extent = extent;
            vkCmdSetScissor(command_buffer, 0, 1, &scissor);

            // ── Dynamic raster + depth state ──────────────────────────
            // Core in Vulkan 1.3. Every state declared dynamic in the pipeline
            // MUST be set before any draw in this command buffer. Dynamic
            // state is command buffer state: it persists across subpasses,
            // and the subpasses below only change what differs.
            //
            // Front face: COUNTER_CLOCKWISE. The projection flips Y for
            // Vulkan's clip space; the specification defines the framebuffer
            // area with a leading minus sign, so geometry wound
            // counter-clockwise when seen from outside (every primitive of
            // Primitive_Builder, and every glTF mesh) is front-facing here.
            vkCmdSetCullMode(command_buffer, raster_state.cull_mode);
            vkCmdSetFrontFace(command_buffer, raster_state.front_face);
            vkCmdSetDepthTestEnable(command_buffer, raster_state.depth_test_enable ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthWriteEnable(command_buffer, raster_state.depth_write_enable ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthCompareOp(command_buffer, raster_state.depth_compare_op);

            // ── Bind descriptor sets ─────────────────────────────────
            // Set 0: per-frame view/projection UBO, light buffer, object
            //        buffer and cluster lists of this frame slot.
            // Set 1: the OIT targets as input attachments, read by the
            //        composite subpass only (graphics contract).
            // Set 2: global material table, indexed through each object's
            //        material_index, and mesh table (bounds view).
            // Set 3: global bindless texture array, indexed through each
            //        material's albedo_texture_index.
            const std::array<VkDescriptorSet, Descriptor_Set::Count> graphics_sets = {
                descriptor_sets[current_frame], composite_input_set, per_material_set, bindless_registry.Get_set() };

            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.Get_handle(),
                Descriptor_Set::Per_Frame, static_cast<uint32_t>(graphics_sets.size()), graphics_sets.data(), 0, nullptr);

            // ── Geometry: one bind for the whole frame ────────────────
            // Every mesh lives in the pool; the draws select theirs through
            // firstIndex / vertexOffset. The composite pipeline declares no
            // vertex input and ignores it.
            geometry_pool.Bind(command_buffer);

            // ── Subpass 0: opaque, depth write on ─────────────────────
            {
                const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Opaque", 0.3f, 0.9f, 0.3f);
                Record_opaque_draws(command_buffer, _frame, opaque_path, bound_pipeline_id, bind_count);
            }

            // ── Subpass 1: transparent accumulation ───────────────────
            // The depth buffer is read-only from here on (its layout in
            // subpasses 1 and 2): every draw must have depth writes
            // disabled. The transparent draws are order-independent: the
            // list needs no back-to-front order.
            vkCmdNextSubpass(command_buffer, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetDepthWriteEnable(command_buffer, VK_FALSE);

            {
                const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Transparent", 0.3f, 0.5f, 1.0f);
                Record_transparent_draws(command_buffer, bound_pipeline_id, bind_count);
            }

            // ── Subpass 2: composite, then debug views ────────────────
            vkCmdNextSubpass(command_buffer, VK_SUBPASS_CONTENTS_INLINE);

            // A frame without transparent draws left the targets at their
            // clear values, whose composite changes nothing: skipped.
            if (!transparent_draws.empty())
            {
                const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "OIT composite", 0.6f, 0.4f, 1.0f);
                Record_composite_draw(command_buffer, bound_pipeline_id, bind_count);
            }

            // Behind the composite: the volumes are drawn over the final
            // color and tested against the opaque depth.
            if (debug_settings.show_bounds)
            {
                const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Bounding volumes", 1.0f, 1.0f, 1.0f);
                Record_bounds_draw(command_buffer, bound_pipeline_id, bind_count);
            }

            vkCmdEndRenderPass(command_buffer);
        }

        if (bind_count != last_reported_binds)
        {
            std::cout << "[Renderer] " << bind_count << " pipeline bind(s) for "
                      << _packet.opaque_items.size() << " opaque + "
                      << _packet.transparent_items.size() << " transparent item(s).\n";
            last_reported_binds = bind_count;
        }

        // ── Statistics readback ───────────────────────────────────
        // The counters of this frame are copied into its host-visible
        // readback buffer, read when this frame slot comes around again
        // (Read_frame_statistics), so the CPU never waits for them. They
        // depend only on the compute -> consumers barrier, so they follow
        // the render pass instead of sitting inside its scope. A label
        // only, not a timed scope: its cost shows as "other".
        {
            const Debug_Label_Scope label(debug_utils, command_buffer, "Statistics readback", 0.5f, 0.5f, 0.5f);

            const VkBufferCopy cluster_copy{ 0, offsetof(Frame_Stats_GPU, cluster_light_references),
                                             sizeof(uint32_t) * 2 };   // light_index_count, dropped_light_count
            const VkBufferCopy draw_copy{ 0, offsetof(Frame_Stats_GPU, gpu_opaque_draws), sizeof(uint32_t) };

            vkCmdCopyBuffer(command_buffer, _frame.cluster_counter_buffer.buffer, _frame.stats_readback_buffer.buffer, 1, &cluster_copy);
            vkCmdCopyBuffer(command_buffer, _frame.gpu_draw_count_buffer.buffer, _frame.stats_readback_buffer.buffer, 1, &draw_copy);

            // Device writes reach host reads only through a barrier with the
            // host as destination; the fence wait then orders the read.
            Vulkan_Buffer_Utils::Record_memory_barrier(command_buffer,
                { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT },
                { VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT });
        }

        // Frame end point, after every command of the frame.
        gpu_timer.End_frame(command_buffer);

        // The driver reports recording errors here, not at the individual
        // vkCmd* calls.
        VK_CHECK(vkEndCommandBuffer(command_buffer), "Record_command_buffer: end");

        // Context of this frame's counters, read back with them. Marked as
        // recorded by Render once the submit succeeded.
        Frame_Record& record = frame_records[current_frame];
        record.recorded = false;
        record.opaque_path = opaque_path;
        record.opaque_objects = opaque_count;
        record.transparent_candidates = transparent_candidates;
        record.transparent_drawn = static_cast<uint32_t>(transparent_draws.size());
        record.procedural_recorded = record_procedural;
    }

    // =========================================================
    // Frame preparation (CPU)
    // =========================================================

    Opaque_Draw_Path Renderer::Resolve_opaque_path(Opaque_Draw_Path _requested, const CoreTypes::RenderPacket& _packet)
    {
        if (_requested != Opaque_Draw_Path::Gpu_Indirect && _requested != Opaque_Draw_Path::Gpu_Culled)
            return _requested;

        // The GPU paths write every command into one bucket, drawn with one
        // pipeline by one vkCmdDrawIndexedIndirectCount.
        bool     single_pipeline = true;
        bool     first_found = false;
        uint8_t  first_pipeline_id = 0;
        uint32_t opaque_items = 0;

        for (const CoreTypes::Draw_Item& item : _packet.opaque_items)
        {
            if ((item.pass_mask & CoreTypes::Render_Pass_Bit::Opaque) == 0)
                continue;

            ++opaque_items;

            const uint8_t pipeline_id = CoreTypes::Get_pipeline_id(item.sort_key);

            if (!first_found)
            {
                first_pipeline_id = pipeline_id;
                first_found = true;
            }
            else if (pipeline_id != first_pipeline_id)
            {
                single_pipeline = false;
            }
        }

        if (single_pipeline && opaque_items <= device.Get_max_draw_indirect_count())
            return _requested;

        if (!warned_gpu_path_fallback)
        {
            std::cerr << "[Renderer] The GPU draw path needs every opaque item on one pipeline and at most maxDrawIndirectCount ("
                << device.Get_max_draw_indirect_count() << ") of them; frames that break it use CPU-written indirect "
                "commands instead. Further occurrences are not reported.\n";
            warned_gpu_path_fallback = true;
        }

        return Opaque_Draw_Path::Cpu_Indirect;
    }

    void Renderer::Prepare_objects(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet, bool _cull_transparents)
    {
        Object_GPU* const objects = static_cast<Object_GPU*>(_frame.object_buffer.mapped_ptr);
        assert(objects != nullptr && "Prepare_objects: the object buffer of the frame is not mapped");

        // Rewritten from entry 0 every frame: the GPU finished reading this
        // slot's copy before its fence was waited on in Render().
        opaque_draws.clear();
        transparent_draws.clear();
        transparent_candidates = 0;

        uint32_t object_count = 0;

        // _subpass: the subpass the list is drawn in; an item whose pipeline
        // was built for another one cannot draw there.
        // _candidates, when not null, counts the valid items of the list
        // before the frustum test.
        const auto add_items = [&](const std::vector<CoreTypes::Draw_Item>& _items, uint8_t _pass_bit, uint32_t _subpass,
                                   std::vector<Draw_Record>& _out, bool _frustum_test, uint32_t* _candidates)
            {
                for (const CoreTypes::Draw_Item& item : _items)
                {
                    // An item that does not take part in this pass is skipped,
                    // so a pass_mask actually selects passes instead of being
                    // decoration.
                    if ((item.pass_mask & _pass_bit) == 0) continue;

                    const uint8_t    item_pipeline_id = CoreTypes::Get_pipeline_id(item.sort_key);
                    const VkPipeline pipeline = pipeline_registry.Get_by_id(item_pipeline_id);

                    // Every reference is validated in every build, before its
                    // entry is written: an out-of-range transform index would
                    // read past the packet's array, an out-of-range material
                    // index past the written slots of the material table, a
                    // released mesh may already have lost its geometry, and a
                    // pipeline of another subpass is invalid in this one.
                    const bool valid =
                        pipeline != VK_NULL_HANDLE &&
                        pipeline_registry.Get_subpass(item_pipeline_id) == _subpass &&
                        item.mesh_gpu_id < meshes.size() &&
                        !meshes[item.mesh_gpu_id].released &&
                        item.transform_idx < _packet.transform_count &&
                        item.material_index < registered_materials.size();

                    if (!valid)
                    {
                        if (!warned_invalid_item)
                        {
                            std::cerr << "[Renderer] Draw item skipped: pipeline " << int(item_pipeline_id)
                                << " (built for subpass " << pipeline_registry.Get_subpass(item_pipeline_id) << ", drawn in subpass "
                                << _subpass << "), mesh " << item.mesh_gpu_id << ", transform " << item.transform_idx
                                << ", material " << item.material_index
                                << " (registered meshes: " << meshes.size() << ", transforms in packet: "
                                << _packet.transform_count << ", registered materials: " << registered_materials.size()
                                << "; a released mesh is also skipped). Further occurrences are not reported.\n";
                            warned_invalid_item = true;
                        }
                        continue;
                    }

                    const MathLib::Matrix4& model = _packet.transforms[item.transform_idx];
                    const Mesh_GPU&         mesh = meshes[item.mesh_gpu_id];

                    if (_candidates != nullptr)
                        ++(*_candidates);

                    // CPU culling: the test of cull_objects.comp, the same
                    // formula on the same planes (culling_frustum is what the
                    // UBO carries): the mesh bounding sphere placed by the
                    // model matrix, an ellipsoid, tested exactly against
                    // every plane, shear included.
                    if (_frustum_test)
                    {
                        if (!culling_frustum.Intersects_ellipsoid(model, MathLib::Vector4(mesh.bounds_center, mesh.bounds_radius)))
                            continue;
                    }

                    // The object buffer is full: this and every later item of
                    // the frame are skipped.
                    if (object_count >= MAX_OBJECTS)
                    {
                        if (!warned_object_overflow)
                        {
                            std::cerr << "[Renderer] More than " << MAX_OBJECTS << " draws in one frame: the rest are skipped. "
                                "Raise MAX_OBJECTS in Frame_Data.hpp. Further occurrences are not reported.\n";
                            warned_object_overflow = true;
                        }
                        return;
                    }

                    // ── Object entry ──────────────────────────────────
                    // Its index is the draw's firstInstance, which the vertex
                    // shader receives as gl_InstanceIndex. No push constants:
                    // the shaders read everything per draw from this entry,
                    // the material table and the mesh table.
                    const uint32_t object_index = object_count++;

                    Object_GPU& object = objects[object_index];
                    object.model = model;
                    // Inverse-transpose computed once per draw here instead of
                    // once per vertex in mesh.vert; correct under non-uniform
                    // scale.
                    object.normal_matrix = glm::transpose(glm::inverse(model));
                    object.material_index = item.material_index;
                    object.mesh_index = item.mesh_gpu_id;
                    object.flags = (static_cast<uint32_t>(item.pass_mask) & Object_Flag::Pass_Mask) | Object_Flag::Active;
                    object._padding0 = 0;

                    _out.push_back({ object_index, item.mesh_gpu_id, item_pipeline_id });
                }
            };

        // Opaque items take entries [0, opaque) and transparent items
        // continue from there. The culling pass relies on it: it processes
        // [0, opaque) only.
        add_items(_packet.opaque_items, CoreTypes::Render_Pass_Bit::Opaque, Render_Subpass::Opaque,
                  opaque_draws, false, nullptr);
        add_items(_packet.transparent_items, CoreTypes::Render_Pass_Bit::Transparent, Render_Subpass::Transparent,
                  transparent_draws, _cull_transparents, &transparent_candidates);
    }

    void Renderer::Write_cpu_draw_commands(Frame_Data& _frame) const
    {
        VkDrawIndexedIndirectCommand* const commands = static_cast<VkDrawIndexedIndirectCommand*>(_frame.cpu_draw_command_buffer.mapped_ptr);
        assert(commands != nullptr && "Write_cpu_draw_commands: the command buffer of the frame is not mapped");

        // Same order as opaque_draws, so the commands of one pipeline are
        // contiguous (the list is sorted by pipeline through the sort key).
        // Host-coherent memory written before the submit: no barrier.
        for (size_t i = 0; i < opaque_draws.size(); ++i)
            commands[i] = meshes[opaque_draws[i].mesh_id].Make_indirect_command(opaque_draws[i].object_index);
    }

    void Renderer::Record_cluster_aabb_update(VkCommandBuffer _command_buffer, const CoreTypes::RenderPacket& _packet)
    {
        const float near_plane = _packet.view.near_plane;

        if (cluster_aabbs_valid && _packet.view.projection == cluster_aabb_projection && near_plane == cluster_aabb_near)
            return;

        Cluster_Grid::Build_aabbs(_packet.view.projection, Cluster_Grid::Make_slice_mapping(near_plane), cluster_aabb_scratch.data());

        // Write-after-read: the cluster pass of earlier frames, possibly
        // still executing, reads the single copy of the boxes. An
        // execution dependency is enough: nothing written before has to
        // become visible to the update.
        Vulkan_Buffer_Utils::Record_memory_barrier(_command_buffer,
            { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0 },
            { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT });

        // vkCmdUpdateBuffer copies the data into the command buffer at
        // record time, so the CPU array is free again as soon as this
        // returns. At most 65536 bytes per call.
        const uint8_t* const data = reinterpret_cast<const uint8_t*>(cluster_aabb_scratch.data());
        const VkDeviceSize   total_size = sizeof(Cluster_AABB_GPU) * CLUSTER_COUNT;

        for (VkDeviceSize offset = 0; offset < total_size; offset += UPDATE_BUFFER_MAX_BYTES)
        {
            const VkDeviceSize chunk = std::min(UPDATE_BUFFER_MAX_BYTES, total_size - offset);
            vkCmdUpdateBuffer(_command_buffer, cluster_aabb_buffer.buffer, offset, chunk, data + offset);
        }

        // Made visible to the cluster pass by the barrier that follows the
        // counter resets, which covers every transfer write before it.

        cluster_aabb_projection = _packet.view.projection;
        cluster_aabb_near = near_plane;
        cluster_aabbs_valid = true;
    }

    // =========================================================
    // Draw recording
    // =========================================================

    void Renderer::Bind_graphics_pipeline(VkCommandBuffer _command_buffer, uint8_t _pipeline_id,
                                          uint8_t& _bound_pipeline_id, uint32_t& _bind_count)
    {
        if (_pipeline_id == _bound_pipeline_id)
            return;

        // Every id reaching here was validated by Prepare_objects, or is
        // one of the Renderer's own pipelines.
        vkCmdBindPipeline(_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_registry.Get_by_id(_pipeline_id));
        _bound_pipeline_id = _pipeline_id;
        ++_bind_count;
    }

    void Renderer::Record_opaque_draws(VkCommandBuffer _command_buffer, Frame_Data& _frame, Opaque_Draw_Path _path,
                                       uint8_t& _bound_pipeline_id, uint32_t& _bind_count)
    {
        if (opaque_draws.empty())
            return;

        switch (_path)
        {
        case Opaque_Draw_Path::Direct:
        {
            // One draw per object, straight from the pool.
            for (const Draw_Record& draw : opaque_draws)
            {
                Bind_graphics_pipeline(_command_buffer, draw.pipeline_id, _bound_pipeline_id, _bind_count);
                meshes[draw.mesh_id].Draw(_command_buffer, draw.object_index);
            }
            break;
        }

        case Opaque_Draw_Path::Cpu_Indirect:
        {
            // One vkCmdDrawIndexedIndirect per run of commands sharing a
            // pipeline, split at maxDrawIndirectCount.
            const size_t max_per_call = std::max<uint32_t>(1u, device.Get_max_draw_indirect_count());
            size_t       run_begin = 0;

            while (run_begin < opaque_draws.size())
            {
                const uint8_t pipeline_id = opaque_draws[run_begin].pipeline_id;
                size_t        run_end = run_begin + 1;

                while (run_end < opaque_draws.size() && opaque_draws[run_end].pipeline_id == pipeline_id)
                    ++run_end;

                Bind_graphics_pipeline(_command_buffer, pipeline_id, _bound_pipeline_id, _bind_count);

                for (size_t first = run_begin; first < run_end; first += max_per_call)
                {
                    const uint32_t draw_count = static_cast<uint32_t>(std::min(max_per_call, run_end - first));

                    vkCmdDrawIndexedIndirect(_command_buffer, _frame.cpu_draw_command_buffer.buffer,
                        static_cast<VkDeviceSize>(first) * DRAW_COMMAND_STRIDE, draw_count, DRAW_COMMAND_STRIDE);
                }

                run_begin = run_end;
            }
            break;
        }

        case Opaque_Draw_Path::Gpu_Indirect:
        case Opaque_Draw_Path::Gpu_Culled:
        {
            // One bucket (Resolve_opaque_path guarantees a single pipeline).
            // The culling pass wrote draw_count commands; maxDrawCount is an
            // upper bound, not the number drawn.
            Bind_graphics_pipeline(_command_buffer, opaque_draws.front().pipeline_id, _bound_pipeline_id, _bind_count);

            vkCmdDrawIndexedIndirectCount(_command_buffer,
                _frame.gpu_draw_command_buffer.buffer, 0,
                _frame.gpu_draw_count_buffer.buffer, offsetof(Draw_Count_GPU, draw_count),
                static_cast<uint32_t>(opaque_draws.size()), DRAW_COMMAND_STRIDE);
            break;
        }

        default:
            break;
        }
    }

    void Renderer::Record_transparent_draws(VkCommandBuffer _command_buffer, uint8_t& _bound_pipeline_id, uint32_t& _bind_count)
    {
        if (transparent_draws.empty())
            return;

        // Weighted blended OIT: every fragment is accumulated into the two
        // targets, so the result does not depend on the draw order and the
        // list needs no back-to-front sort. It arrives grouped by pipeline,
        // material and mesh, like the opaque one, which keeps the binds to
        // a minimum. The opaque depth occludes the fragments; depth writes
        // were disabled when the subpass began.
        for (const Draw_Record& draw : transparent_draws)
        {
            Bind_graphics_pipeline(_command_buffer, draw.pipeline_id, _bound_pipeline_id, _bind_count);
            meshes[draw.mesh_id].Draw(_command_buffer, draw.object_index);
        }
    }

    void Renderer::Record_composite_draw(VkCommandBuffer _command_buffer, uint8_t& _bound_pipeline_id, uint32_t& _bind_count)
    {
        Bind_graphics_pipeline(_command_buffer, composite_pipeline_id, _bound_pipeline_id, _bind_count);

        // Every pixel is resolved, whatever the opaque depth holds, and the
        // full-screen triangle is not culled by its winding.
        vkCmdSetDepthTestEnable(_command_buffer, VK_FALSE);
        vkCmdSetCullMode(_command_buffer, VK_CULL_MODE_NONE);

        // Three vertices generated from gl_VertexIndex (oit_composite.vert);
        // the targets are read through set 1 as input attachments.
        vkCmdDraw(_command_buffer, 3, 1, 0, 0);
    }

    void Renderer::Record_bounds_draw(VkCommandBuffer _command_buffer, uint8_t& _bound_pipeline_id, uint32_t& _bind_count)
    {
        // Entries [0, object_count) of the object buffer: the opaque objects
        // and the transparent ones that survived the CPU culling. Opaque
        // objects culled on the GPU keep their volume, which is what shows
        // the culling at work.
        const uint32_t object_count = static_cast<uint32_t>(opaque_draws.size() + transparent_draws.size());

        if (object_count == 0 || bounds_sphere_mesh_id >= meshes.size())
            return;

        const Mesh_GPU& sphere = meshes[bounds_sphere_mesh_id];

        if (sphere.released || !sphere.geometry.Is_valid())
            return;

        Bind_graphics_pipeline(_command_buffer, bounds_pipeline_id, _bound_pipeline_id, _bind_count);

        // Seen from inside as well, never occluding what follows, and
        // hidden behind the opaque geometry: the composite may have turned
        // the depth test off. Depth writes stay off, as the read-only depth
        // layout of the subpass requires.
        vkCmdSetCullMode(_command_buffer, VK_CULL_MODE_NONE);
        vkCmdSetDepthTestEnable(_command_buffer, VK_TRUE);
        vkCmdSetDepthWriteEnable(_command_buffer, VK_FALSE);

        // One instanced draw: firstInstance 0 and one instance per object
        // entry, so gl_InstanceIndex is the object index in bounds.vert.
        vkCmdDrawIndexed(_command_buffer, sphere.geometry.index_count, object_count, sphere.geometry.first_index,
            static_cast<int32_t>(sphere.geometry.first_vertex), 0);
    }

    // =========================================================
    // Recreate_swapchain
    // =========================================================

    void Renderer::Recreate_swapchain()
    {
        VK_CHECK(vkDeviceWaitIdle(device.Get_logical_device_handle()),
            "Recreate_swapchain: wait for device idle");

        // Every submitted frame has completed: released geometry can go.
        completed_frames = submitted_frames;
        Free_retired_meshes();

        // vkDeviceWaitIdle covers queue work, not presentation: the
        // per-image objects are retired through their present fences (or
        // a grace period without them), never reset while pending.
        Retire_image_sync();

        swapchain.Recreate();
        depth_resources.Recreate(swapchain.Get_extent());
        oit_resources.Recreate(swapchain.Get_extent());
        framebuffers.Recreate(render_pass, swapchain, depth_resources, oit_resources);

        // The OIT views changed: the composite reads the new ones. Legal
        // here, after the idle wait: no pending command buffer uses the set.
        Write_composite_input_set();
        Name_oit_targets();

        // The cluster boxes need nothing here: they depend on the
        // projection, and the next frame rebuilds them when the new aspect
        // ratio changes it (Record_cluster_aabb_update). The grid itself
        // has a fixed tile count, so no buffer depends on the resolution.
        //
        // Resolution-dependent images registered in the bindless set (the
        // Storage_Image outputs of compute passes) belong here as well,
        // after the idle wait above: Storage_Image::Recreate, with its
        // clear submitted before the next frame reads the slot, then
        // Bindless_Registry::Update_texture on the slot the image already
        // holds. Every draw keeps its index and reads a live view, and no
        // slot is consumed per resize. A recreated image holds zeros: the
        // pass that writes it must be marked out of date, as
        // procedural_dirty does for the procedural texture.

        // The image count may have changed: one Image_Sync per new image.
        Create_image_sync();

        framebuffer_resized = false;

        std::cout << "[Renderer] Swapchain recreated (" << swapchain.Get_image_count() << " images).\n";
    }

    // =========================================================
    // Submit_and_wait_transfer
    // =========================================================
    // The CPU must not free the staging buffers until the GPU has consumed
    // them, so this blocks, and a CPU wait means a fence. vkQueueWaitIdle
    // would also work, but it waits for the WHOLE graphics queue, stalling
    // any frame already in flight. The fence waits only for this submit.

    void Renderer::Submit_and_wait_transfer(VkCommandBuffer _transfer_cmd)
    {
        if (_transfer_cmd == VK_NULL_HANDLE)
            throw std::invalid_argument("Submit_and_wait_transfer: null command buffer");

        VkDevice dev = device.Get_logical_device_handle();

        VK_CHECK(vkEndCommandBuffer(_transfer_cmd), "Submit_and_wait_transfer: failed to end command buffer");

        // The fence is shared across uploads: unsignal it before reuse.
        VK_CHECK(vkResetFences(dev, 1, &transfer_fence), "Submit_and_wait_transfer: reset transfer fence");

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &_transfer_cmd;

        VK_CHECK(vkQueueSubmit(device.Get_graphics_queue(), 1, &submit_info, transfer_fence),
            "Submit_and_wait_transfer: failed to submit");

        VK_CHECK(vkWaitForFences(dev, 1, &transfer_fence, VK_TRUE, UINT64_MAX),
            "Submit_and_wait_transfer: wait for transfer fence");
    }

    // =========================================================
    // Init_descriptor_pool
    // =========================================================

    void Renderer::Init_descriptor_pool()
    {
        // Set 0: one uniform buffer and seven storage buffers (lights,
        // objects, cluster grid, cluster light indices, cluster counters,
        // draw commands, draw count) per frame-in-flight. Set 1 of the
        // compute contract: one storage image for the procedural pass and
        // one storage buffer for the cluster boxes (per_pass_set). Set 1 of
        // the graphics contract: two input attachments, the OIT targets
        // (composite_input_set). Set 2: two storage buffers, the material
        // and mesh tables (per_material_set). Sets 1 and 2 are shared by
        // every frame slot.
        constexpr uint32_t PER_FRAME_STORAGE_BUFFERS = 7;
        constexpr uint32_t PER_PASS_STORAGE_BUFFERS = 1;
        constexpr uint32_t PER_MATERIAL_STORAGE_BUFFERS = 2;
        constexpr uint32_t COMPOSITE_INPUT_ATTACHMENTS = 2;

        std::array<VkDescriptorPoolSize, 4> pool_sizes{};

        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_sizes[0].descriptorCount = FRAMES_IN_FLIGHT;

        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[1].descriptorCount = PER_FRAME_STORAGE_BUFFERS * FRAMES_IN_FLIGHT + PER_PASS_STORAGE_BUFFERS + PER_MATERIAL_STORAGE_BUFFERS;

        pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pool_sizes[2].descriptorCount = 1;

        pool_sizes[3].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        pool_sizes[3].descriptorCount = COMPOSITE_INPUT_ATTACHMENTS;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        // One set 0 per frame slot + the compute set 1 + the graphics set 1
        // + set 2.
        pool_info.maxSets = FRAMES_IN_FLIGHT + 3;

        VK_CHECK(vkCreateDescriptorPool(device.Get_logical_device_handle(), &pool_info, nullptr, &descriptor_pool),
            "Renderer: failed to create descriptor pool");
    }

    // =========================================================
    // Composite input set (graphics set 1)
    // =========================================================

    void Renderer::Init_composite_input_set()
    {
        const VkDescriptorSetLayout composite_layout = descriptor_layouts.Get(Descriptor_Set::Per_Pass, Pipeline_Kind::Graphics);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &composite_layout;

        VK_CHECK(vkAllocateDescriptorSets(device.Get_logical_device_handle(), &alloc_info, &composite_input_set),
            "Init_composite_input_set: failed to allocate the graphics set 1");

        Write_composite_input_set();
    }

    void Renderer::Write_composite_input_set()
    {
        assert(composite_input_set != VK_NULL_HANDLE && "Write_composite_input_set: the set is not allocated");

        // SHADER_READ_ONLY_OPTIMAL: the layout the composite subpass reads
        // them in (the input attachment references of Vulkan_Render_Pass).
        // No sampler: subpassLoad reads the texel of the current pixel.
        std::array<VkDescriptorImageInfo, 2> image_infos{};

        image_infos[0].sampler = VK_NULL_HANDLE;
        image_infos[0].imageView = oit_resources.Get_accumulation_view();
        image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        image_infos[1].sampler = VK_NULL_HANDLE;
        image_infos[1].imageView = oit_resources.Get_revealage_view();
        image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const std::array<uint32_t, 2> bindings = { Binding_Graphics_Pass::Oit_Accumulation, Binding_Graphics_Pass::Oit_Revealage };

        std::array<VkWriteDescriptorSet, 2> writes{};

        for (size_t i = 0; i < writes.size(); ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = composite_input_set;
            writes[i].dstBinding = bindings[i];
            writes[i].dstArrayElement = 0;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo = &image_infos[i];
        }

        vkUpdateDescriptorSets(device.Get_logical_device_handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // =========================================================
    // Init_descriptor_sets
    // =========================================================

    void Renderer::Init_descriptor_sets()
    {
        // Set 0 is shared by both contracts.
        std::array<VkDescriptorSetLayout, FRAMES_IN_FLIGHT> layouts;
        layouts.fill(descriptor_layouts.Get(Descriptor_Set::Per_Frame, Pipeline_Kind::Graphics));

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = FRAMES_IN_FLIGHT;
        alloc_info.pSetLayouts = layouts.data();

        VK_CHECK(vkAllocateDescriptorSets(device.Get_logical_device_handle(), &alloc_info, descriptor_sets.data()),
            "Renderer: failed to allocate descriptor sets");

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            VkDescriptorBufferInfo ubo_info{};
            ubo_info.buffer = frames[i].uniform_buffer.buffer;
            ubo_info.offset = 0;
            ubo_info.range = sizeof(Frame_UBO);

            VkDescriptorBufferInfo light_info{};
            light_info.buffer = frames[i].light_buffer.buffer;
            light_info.offset = 0;
            // The range is the whole CAPACITY, not this frame's live lights:
            // the descriptor is written once at startup and the contents
            // change by memcpy. Frame_UBO::light_count says how many entries
            // are valid.
            light_info.range = sizeof(Light_GPU) * MAX_LIGHTS;

            // Same rule as the lights: the whole capacity. Only the entries
            // written this frame are read, because every draw indexes its own
            // entry through firstInstance.
            VkDescriptorBufferInfo object_info{};
            object_info.buffer = frames[i].object_buffer.buffer;
            object_info.offset = 0;
            object_info.range = sizeof(Object_GPU) * MAX_OBJECTS;

            // Buffers written by the compute passes of this frame slot,
            // whole capacity as well.
            VkDescriptorBufferInfo cluster_grid_info{};
            cluster_grid_info.buffer = frames[i].cluster_grid_buffer.buffer;
            cluster_grid_info.offset = 0;
            cluster_grid_info.range = sizeof(Cluster_Range_GPU) * CLUSTER_COUNT;

            VkDescriptorBufferInfo cluster_indices_info{};
            cluster_indices_info.buffer = frames[i].cluster_light_index_buffer.buffer;
            cluster_indices_info.offset = 0;
            cluster_indices_info.range = sizeof(uint32_t) * CLUSTER_LIGHT_INDEX_CAPACITY;

            VkDescriptorBufferInfo cluster_counters_info{};
            cluster_counters_info.buffer = frames[i].cluster_counter_buffer.buffer;
            cluster_counters_info.offset = 0;
            cluster_counters_info.range = sizeof(Cluster_Counters_GPU);

            VkDescriptorBufferInfo draw_commands_info{};
            draw_commands_info.buffer = frames[i].gpu_draw_command_buffer.buffer;
            draw_commands_info.offset = 0;
            draw_commands_info.range = sizeof(VkDrawIndexedIndirectCommand) * MAX_OBJECTS;

            VkDescriptorBufferInfo draw_count_info{};
            draw_count_info.buffer = frames[i].gpu_draw_count_buffer.buffer;
            draw_count_info.offset = 0;
            draw_count_info.range = sizeof(Draw_Count_GPU);

            // Binding and buffer of every descriptor of set 0, in binding
            // order; binding 0 is the uniform buffer, the rest are storage
            // buffers.
            constexpr size_t PER_FRAME_BINDING_COUNT = 8;

            const std::array<std::pair<uint32_t, const VkDescriptorBufferInfo*>, PER_FRAME_BINDING_COUNT> bindings = { {
                { Binding_Per_Frame::Frame_UBO,             &ubo_info },
                { Binding_Per_Frame::Lights,                &light_info },
                { Binding_Per_Frame::Objects,               &object_info },
                { Binding_Per_Frame::Cluster_Grid,          &cluster_grid_info },
                { Binding_Per_Frame::Cluster_Light_Indices, &cluster_indices_info },
                { Binding_Per_Frame::Cluster_Counters,      &cluster_counters_info },
                { Binding_Per_Frame::Draw_Commands,         &draw_commands_info },
                { Binding_Per_Frame::Draw_Count,            &draw_count_info } } };

            std::array<VkWriteDescriptorSet, PER_FRAME_BINDING_COUNT> writes{};

            for (size_t b = 0; b < PER_FRAME_BINDING_COUNT; ++b)
            {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = descriptor_sets[i];
                writes[b].dstBinding = bindings[b].first;
                writes[b].dstArrayElement = 0;
                writes[b].descriptorType = (bindings[b].first == Binding_Per_Frame::Frame_UBO)
                                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[b].descriptorCount = 1;
                writes[b].pBufferInfo = bindings[b].second;
            }

            vkUpdateDescriptorSets(device.Get_logical_device_handle(),
                static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    // =========================================================
    // Statistics
    // =========================================================

    void Renderer::Read_frame_statistics(uint32_t _frame_slot)
    {
        Frame_Record& record = frame_records[_frame_slot];

        // Nothing submitted from this slot since its last read.
        if (!record.recorded)
            return;

        record.recorded = false;

        Frame_Stats_GPU counters{};
        Vulkan_Buffer_Utils::Read_from_buffer(allocator.Get_handle(), frames[_frame_slot].stats_readback_buffer, &counters, sizeof(counters));

        // Reported whether statistics are printed or not: a full list
        // silently removes lights from clusters.
        if (counters.cluster_lights_dropped > 0 && !warned_cluster_overflow)
        {
            std::cerr << "[Renderer] The cluster light index list overflowed: " << counters.cluster_light_references
                << " entries requested, " << CLUSTER_LIGHT_INDEX_CAPACITY << " available; " << counters.cluster_lights_dropped
                << " light references dropped (magenta in the heatmap). Raise CLUSTER_AVERAGE_LIGHTS or lower the light ranges. "
                "Further occurrences are not reported.\n";
            warned_cluster_overflow = true;
        }

        Frame_Statistics& stats = statistics;

        ++stats.frames;

        // A frame measured in the other timing mode (the switch changed
        // while it was in flight) is not mixed into the averages.
        if (gpu_timer.Read_frame(_frame_slot, timer_frame) && timer_frame.isolated == debug_settings.isolate_gpu_timings)
        {
            ++stats.timed_frames;
            stats.total_ms_sum += timer_frame.total_ms;
            stats.other_ms_sum += timer_frame.total_ms - timer_frame.top_level_ms;

            Accumulate_scope_timings();
        }

        stats.last_counters = counters;
        stats.last_record = record;

        const auto now = std::chrono::steady_clock::now();

        if (now - stats.last_print < STATISTICS_PRINT_INTERVAL)
            return;

        if (debug_settings.print_stats)
        {
            const std::ios_base::fmtflags previous_flags = std::cout.flags();
            const std::streamsize         previous_precision = std::cout.precision();

            std::cout << std::fixed << std::setprecision(3);

            Print_gpu_timings();

            const Frame_Record& last = stats.last_record;
            const bool gpu_path = last.opaque_path == Opaque_Draw_Path::Gpu_Indirect || last.opaque_path == Opaque_Draw_Path::Gpu_Culled;

            // The GPU paths report what the culling pass kept; the CPU paths
            // draw every object they wrote.
            const uint32_t opaque_drawn = gpu_path ? stats.last_counters.gpu_opaque_draws : last.opaque_objects;

            std::cout << "[Renderer] Opaque drawn " << opaque_drawn << " / " << last.opaque_objects << " (" << To_string(last.opaque_path)
                << ") | transparent drawn " << last.transparent_drawn << " / " << last.transparent_candidates
                << " | cluster light references " << stats.last_counters.cluster_light_references << " / " << CLUSTER_LIGHT_INDEX_CAPACITY
                << " (dropped " << stats.last_counters.cluster_lights_dropped << ") | geometry pool "
                << geometry_pool.Get_used_vertices() << " / " << geometry_pool.Get_vertex_capacity() << " vertices, "
                << geometry_pool.Get_used_indices() << " / " << geometry_pool.Get_index_capacity() << " indices\n";

            std::cout.flags(previous_flags);
            std::cout.precision(previous_precision);
        }

        Reset_statistics();
        stats.last_print = now;
    }

    void Renderer::Accumulate_scope_timings()
    {
        std::vector<Scope_Statistics>& accumulated = statistics.scopes;

        // Merged in recording order: a scope already known keeps its place,
        // a new one goes right after the scope that preceded it in this
        // frame, so the print follows the order of the frame even when a
        // scope (procedural, culling) is missing from the first frames.
        size_t insert_at = 0;

        for (const Gpu_Scope_Timing& timing : timer_frame.scopes)
        {
            auto it = std::find_if(accumulated.begin(), accumulated.end(), [&](const Scope_Statistics& _scope)
                {
                    return Same_scope_name(_scope.name, timing.name) && Same_scope_name(_scope.parent, timing.parent);
                });

            if (it == accumulated.end())
            {
                Scope_Statistics scope;
                scope.name = timing.name;
                scope.parent = timing.parent;
                scope.depth = timing.depth;

                it = accumulated.insert(accumulated.begin() + static_cast<std::ptrdiff_t>(std::min(insert_at, accumulated.size())), scope);
            }

            ++it->frames;
            it->ms_sum += timing.ms;

            insert_at = static_cast<size_t>(it - accumulated.begin()) + 1;
        }
    }

    void Renderer::Print_gpu_timings() const
    {
        const Frame_Statistics& stats = statistics;

        if (stats.timed_frames == 0)
        {
            std::cout << "[Renderer] GPU timings unavailable (no timestamp support, or no frame measured yet).\n";
            return;
        }

        const double frames = static_cast<double>(stats.timed_frames);

        // Every average is taken over all the measured frames, a scope
        // missing from a frame counting 0 there, so the top-level scopes
        // plus "other" add up to the total. A scope recorded in only some
        // of the frames also shows its cost in those frames.
        const auto print_scope = [&](const Scope_Statistics& _scope)
            {
                std::cout << (_scope.name ? _scope.name : "?") << " " << (_scope.ms_sum / frames);

                if (_scope.frames < stats.timed_frames && _scope.frames > 0)
                {
                    std::cout << " (in " << _scope.frames << " of " << stats.timed_frames << " frames, "
                        << (_scope.ms_sum / static_cast<double>(_scope.frames)) << " each)";
                }
            };

        // Nested scopes follow their parent in brackets, at any depth.
        const auto print_children = [&](const auto& _self, const Scope_Statistics& _parent) -> void
            {
                bool first = true;

                for (const Scope_Statistics& child : stats.scopes)
                {
                    if (child.depth != _parent.depth + 1 || !Same_scope_name(child.parent, _parent.name))
                        continue;

                    std::cout << (first ? " [" : ", ");
                    print_scope(child);
                    _self(_self, child);
                    first = false;
                }

                if (!first)
                    std::cout << "]";
            };

        std::cout << "[Renderer] GPU ms";

        if (debug_settings.isolate_gpu_timings)
            std::cout << ", ISOLATED scopes (a full barrier before each: every scope measures its pass alone, the total is not a frame time)";

        std::cout << ", average of " << stats.timed_frames << " frames: ";

        for (const Scope_Statistics& scope : stats.scopes)
        {
            if (scope.depth != 0)
                continue;

            print_scope(scope);
            print_children(print_children, scope);
            std::cout << " | ";
        }

        // Barriers between scopes, the statistics copies and gaps.
        std::cout << "other " << (stats.other_ms_sum / frames) << " | total " << (stats.total_ms_sum / frames) << "\n";
    }

    void Renderer::Reset_statistics()
    {
        statistics.frames = 0;
        statistics.timed_frames = 0;
        statistics.total_ms_sum = 0.0;
        statistics.other_ms_sum = 0.0;
        statistics.scopes.clear();
    }

    // =========================================================
    // Debug switches
    // =========================================================

    void Renderer::Set_debug_settings(const Render_Debug_Settings& _settings)
    {
        Render_Debug_Settings settings = _settings;

        // Values that reach the shaders or select a code path are sanitized:
        // an unknown enumerator falls back to the default.
        if (static_cast<uint32_t>(settings.light_culling) >= static_cast<uint32_t>(Light_Culling_Mode::Count))
            settings.light_culling = Light_Culling_Mode::Clustered;

        if (static_cast<uint32_t>(settings.cluster_view) >= static_cast<uint32_t>(Cluster_Debug_View::Count))
            settings.cluster_view = Cluster_Debug_View::None;

        if (static_cast<uint32_t>(settings.opaque_path) >= static_cast<uint32_t>(Opaque_Draw_Path::Count))
            settings.opaque_path = Opaque_Draw_Path::Gpu_Culled;

        if (settings.heatmap_max_lights == 0)
            settings.heatmap_max_lights = 1;

        if (settings.light_culling != debug_settings.light_culling)
            std::cout << "[Renderer] Light culling: " << To_string(settings.light_culling) << ".\n";

        if (settings.cluster_view != debug_settings.cluster_view)
            std::cout << "[Renderer] Cluster debug view: " << To_string(settings.cluster_view) << ".\n";

        if (settings.heatmap_max_lights != debug_settings.heatmap_max_lights)
            std::cout << "[Renderer] Heatmap: red at " << settings.heatmap_max_lights << " lights per cluster.\n";

        if (settings.opaque_path != debug_settings.opaque_path)
            std::cout << "[Renderer] Opaque draw path: " << To_string(settings.opaque_path) << ".\n";

        if (settings.freeze_culling != debug_settings.freeze_culling)
        {
            // The frustum of the next frame becomes the frozen one.
            if (settings.freeze_culling)
                capture_frozen_frustum = true;

            std::cout << "[Renderer] Culling camera " << (settings.freeze_culling ? "frozen" : "follows the view") << ".\n";
        }

        if (settings.show_bounds != debug_settings.show_bounds)
            std::cout << "[Renderer] Bounding volumes " << (settings.show_bounds ? "shown" : "hidden") << ".\n";

        if (settings.print_stats != debug_settings.print_stats)
        {
            // The first print covers only frames measured from now on.
            Reset_statistics();
            statistics.last_print = std::chrono::steady_clock::now();

            std::cout << "[Renderer] Statistics " << (settings.print_stats ? "printed every second" : "off") << ".\n";
        }

        if (settings.isolate_gpu_timings != debug_settings.isolate_gpu_timings)
        {
            // Frames of the two modes are never averaged together.
            Reset_statistics();
            statistics.last_print = std::chrono::steady_clock::now();

            std::cout << "[Renderer] GPU timing scopes "
                << (settings.isolate_gpu_timings
                    ? "isolated: a full barrier before each top-level scope, which measures its pass alone; frame times are not representative"
                    : "not isolated: overlapping work is charged to the scope that finishes later")
                << ".\n";
        }

        debug_settings = settings;
    }

    // =========================================================
    // Build_pipeline_manifest
    // =========================================================

    std::vector<Pipeline_Config> Renderer::Build_pipeline_manifest() const
    {
        return { opaque_config, transparent_config, bounds_config, composite_config };
    }

} // namespace Renderer_System
