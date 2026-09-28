#include "Renderer_Impl.hpp"

#include <Cluster_Grid.hpp>
#include <Vulkan_Utils.hpp>

#include <MathConstants.hpp>
#include <Matrix4.hpp>
#include <Window.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

// The Renderer as an orchestrator: construction and destruction of the
// Vulkan stack and the subsystems, the frame loop (Render), swapchain
// recreation, the debug switches and the public interface. The work itself
// lives in the subsystems; the rest of Renderer::Impl is spread by
// responsibility:
//   Renderer_Setup.cpp  - descriptor pool and sets, global tables, procedural
//                         pass, debug meshes and names;
//   Renderer_Assets.cpp - uploads, mesh release, materials;
//   Renderer_Passes.cpp - command recording: the sequence of passes and
//                         the draws.

namespace Renderer_System
{

    namespace
    {
        // Frame_UBO::frustum_planes is filled plane by plane from the
        // culling frustum.
        static_assert(FRUSTUM_PLANE_COUNT == CoreTypes::Frustum::PLANE_COUNT,
            "Frame_UBO::frustum_planes and CoreTypes::Frustum must hold the same planes");

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

        // =========================================================
        // Pipeline configurations
        // =========================================================

        Pipeline_Config Make_opaque_config()
        {
            Pipeline_Config config;
            config.vertex_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.vert.spv";
            config.fragment_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.frag.spv";
            config.subpass = Render_Subpass::Opaque;
            config.color_attachment_count = 1;
            config.color_blend[0] = Color_Blend_State{};   // no blending
            return config;
        }

        // _wireframe: VK_POLYGON_MODE_LINE (needs fillModeNonSolid);
        // otherwise filled ellipsoids with alpha blending.
        Pipeline_Config Make_bounds_config(bool _wireframe)
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

        Pipeline_Config Make_transparent_config()
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

        Pipeline_Config Make_composite_config()
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
    }

    // =========================================================
    // Constructor
    // =========================================================

    Renderer::Impl::Impl(const Platform::Window& _window, Validation_Mode _validation_mode)
        : window(_window),
        instance(_validation_mode, "Game", "Engine"),
        surface(instance, _window),
        device(instance, surface),
        allocator(instance, device),
        debug_utils(instance, device),
        geometry_pool(allocator.Get_handle(), GEOMETRY_POOL_VERTICES, GEOMETRY_POOL_INDICES),
        mesh_registry(geometry_pool, MAX_MESHES),
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
        procedural_pipeline(device, pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle(), "..\\..\\Renderer\\shaders\\compiled\\procedural.comp.spv"),
        cull_pipeline(device, pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle(), "..\\..\\Renderer\\shaders\\compiled\\cull_objects.comp.spv"),
        light_clusters(device, allocator.Get_handle(), pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle()),
        opaque_config(Make_opaque_config()),
        transparent_config(Make_transparent_config()),
        bounds_config(Make_bounds_config(device.Is_fill_mode_non_solid_enabled())),
        composite_config(Make_composite_config()),
        sampler_cache(device),
        timeline(FRAMES_IN_FLIGHT),
        upload_context(device, allocator.Get_handle()),
        material_table(allocator.Get_handle(), MAX_MATERIALS),
        statistics(FRAMES_IN_FLIGHT, GPU_TIMER_MAX_SCOPES),
        present_sync(device)
    {
        try
        {
#ifndef NDEBUG
            // The CPU culling of the transparent items and the GPU culling
            // of the opaque ones evaluate the same formula; its known case
            // is checked once per debug run.
            Check_ellipsoid_culling();
#endif

            // -- Bindless samplers --
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

            // -- Frame resources --
            frames.reserve(FRAMES_IN_FLIGHT);
            for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
                frames.emplace_back(device, allocator.Get_handle());

            present_sync.Create(swapchain.Get_image_count());

            // -- Descriptor pool + sets --
            Init_descriptor_pool();
            Init_descriptor_sets();
            Init_composite_input_set();

            // -- Default textures --
            // Needs the upload context. First upload of the session, so the
            // defaults take the reserved bindless slots.
            Upload_default_textures();

            // -- Material and mesh tables --
            // After the defaults: the default material samples
            // Default_Texture::White, and registration checks that its
            // slot exists. After the descriptor pool (per_material_set).
            // Before any mesh upload: uploads write the mesh table.
            Init_global_tables();

            // -- Compute pass resources --
            // After the defaults (its bindless slot comes after theirs) and
            // after the descriptor pool (per_pass_set is allocated from it).
            Init_procedural_pass();

            // After Init_procedural_pass, which allocates per_pass_set.
            Init_light_clusters();

            // After the mesh table exists.
            Init_debug_meshes();

            Name_debug_objects();

            timer_frame.scopes.reserve(GPU_TIMER_MAX_SCOPES);
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

    Renderer::Impl::~Impl()
    {
        // A device that cannot go idle at shutdown (device lost) is not a
        // reason to skip the cleanup that is still possible.
        const VkResult idle = vkDeviceWaitIdle(device.Get_logical_device_handle());
        if (idle != VK_SUCCESS)
            std::cerr << "[Renderer] vkDeviceWaitIdle failed at shutdown: " << Vulkan_Utils::Vk_result_to_string(idle) << "\n";

        Destroy_owned_handles();

        // The rest is destroyed in reverse construction order by the
        // destructors of the members (RAII). Present_Sync retires and
        // destroys the presentation objects, and sampler_cache destroys all
        // cached VkSampler handles.
    }

    void Renderer::Impl::Destroy_owned_handles()
    {
        VkDevice dev = device.Get_logical_device_handle();

        // Assets first: they hold GPU buffers/images that may still be
        // referenced by in-flight command buffers otherwise. Mesh records
        // own no memory: their ranges are released with the Geometry_Pool.
        textures.clear();
        procedural_image.reset();

        // Tolerates a buffer that was never created (constructor failure
        // before Init_global_tables).
        Vulkan_Buffer_Utils::Destroy_buffer(allocator.Get_handle(), mesh_table_buffer);

        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(dev, descriptor_pool, nullptr);
            descriptor_pool = VK_NULL_HANDLE;
        }

        frames.clear();
    }

    // =========================================================
    // Surface size
    // =========================================================

    bool Renderer::Impl::Recreate_swapchain_if_needed()
    {
        if (!framebuffer_resized) return true;

        // A minimized window has no framebuffer to build a swapchain for.
        // The flag stays set; the next call after restoring handles it.
        if (window.Is_minimized()) return false;

        Recreate_swapchain();
        return true;
    }

    // =========================================================
    // Render
    // =========================================================

    void Renderer::Impl::Render(const CoreTypes::RenderPacket& _packet)
    {
        VkDevice dev = device.Get_logical_device_handle();

        // A resize reported by the window is applied before the frame, so
        // the frame is rendered at the new size. While minimized there is
        // nothing to render into.
        if (framebuffer_resized && !Recreate_swapchain_if_needed()) return;

        Frame_Data& frame = frames[current_frame];

        // -- Wait for this frame slot to be free --
        // A device loss surfaces here as an exception instead of the loop
        // submitting forever to a dead device.
        VK_CHECK(vkWaitForFences(dev, 1, &frame.in_flight_fence, VK_TRUE, UINT64_MAX),
            "Render: wait for frame fence");

        // -- Work of the previous use of this slot --
        // Its fence is signaled: that frame and every earlier submission
        // have completed (a fence signal includes all the work submitted
        // before it). Geometry released before then can go back to the
        // pool, and the GPU results of that frame can be read.
        timeline.Mark_slot_complete(current_frame);
        mesh_registry.Free_completed(timeline);
        Read_frame_statistics(current_frame);

        // -- Acquire swapchain image --
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

        Present_Sync::Image_Sync& sync = present_sync.Get(image_index);

        // -- Wait for this image's previous present to complete --
        present_sync.Wait_for_previous_present(image_index);

        // -- Update per-frame buffers --
        draw_list.Update_culling_frustum(_packet.view.frustum, debug_settings.freeze_culling);
        Write_frame_uniforms(frame, _packet);

        // -- Record commands --
        frame.command_pool.Reset_command_buffer(0);
        Record_command_buffer(frame, _packet, image_index);

        // -- Reset fence just before submit (not before recording) --
        // Only a submit signals the fence again. Were it reset before
        // recording, an exception thrown while recording would leave it
        // unsignaled with no submit pending, and the next
        // vkWaitForFences(UINT64_MAX) on this frame slot would never
        // return.
        VK_CHECK(vkResetFences(dev, 1, &frame.in_flight_fence), "Render: reset frame fence");

        // -- Submit --
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
        timeline.Record_submission(current_frame);
        statistics.Mark_submitted(current_frame);

        // The procedural image this submission writes stays valid for every
        // later frame (End_write makes it visible to all later work on the
        // queue). Cleared only here, after a successful submit: a recording
        // that threw or a failed submit leaves the flag set, and the next
        // frame records the pass again.
        if (statistics.Recorded_procedural(current_frame))
            procedural_dirty = false;

        // -- Present --
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

        present_sync.Flush(false);

        current_frame = (current_frame + 1) % FRAMES_IN_FLIGHT;
    }

    // =========================================================
    // Write_frame_uniforms
    // =========================================================

    void Renderer::Impl::Write_frame_uniforms(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet)
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

        // -- Clustered lighting --
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

        // -- Culling --
        // The frustum of this frame, or the frozen one.
        const CoreTypes::Frustum& culling_frustum = draw_list.Get_culling_frustum();

        for (uint32_t i = 0; i < FRUSTUM_PLANE_COUNT; ++i)
            ubo.frustum_planes[i] = culling_frustum.planes[i];

        std::memcpy(_frame.uniform_buffer.mapped_ptr, &ubo, sizeof(ubo));
    }

    // =========================================================
    // Recreate_swapchain
    // =========================================================

    void Renderer::Impl::Recreate_swapchain()
    {
        VK_CHECK(vkDeviceWaitIdle(device.Get_logical_device_handle()),
            "Recreate_swapchain: wait for device idle");

        // Every submitted frame has completed: released geometry can go.
        timeline.Mark_all_complete();
        mesh_registry.Free_completed(timeline);

        // vkDeviceWaitIdle covers queue work, not presentation: the
        // per-image objects are retired through their present fences (or
        // a grace period without them), never reset while pending.
        present_sync.Retire();

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
        // ratio changes it (Light_Clusters::Record_aabb_update). The grid
        // itself has a fixed tile count, so no buffer depends on the
        // resolution.
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
        present_sync.Create(swapchain.Get_image_count());

        framebuffer_resized = false;

        std::cout << "[Renderer] Swapchain recreated (" << swapchain.Get_image_count() << " images).\n";
    }

    // =========================================================
    // Statistics
    // =========================================================

    void Renderer::Impl::Read_frame_statistics(uint32_t _frame_slot)
    {
        Frame_Statistics::Frame_Record record;

        // Nothing submitted from this slot since its last read.
        if (!statistics.Take_pending(_frame_slot, record))
            return;

        const Frame_Stats_GPU counters = Frame_Statistics::Read_counters(allocator.Get_handle(), frames[_frame_slot].stats_readback_buffer);

        const bool timed = gpu_timer.Read_frame(_frame_slot, timer_frame);

        statistics.Accumulate(record, counters, timed ? &timer_frame : nullptr, debug_settings.isolate_gpu_timings);

        Frame_Statistics::Geometry_Usage geometry;
        geometry.used_vertices = geometry_pool.Get_used_vertices();
        geometry.vertex_capacity = geometry_pool.Get_vertex_capacity();
        geometry.used_indices = geometry_pool.Get_used_indices();
        geometry.index_capacity = geometry_pool.Get_index_capacity();

        statistics.Report(debug_settings.print_stats, debug_settings.isolate_gpu_timings, geometry);
    }

    // =========================================================
    // Debug switches
    // =========================================================

    void Renderer::Impl::Set_debug_settings(const Render_Debug_Settings& _settings)
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
                draw_list.Request_frustum_capture();

            std::cout << "[Renderer] Culling camera " << (settings.freeze_culling ? "frozen" : "follows the view") << ".\n";
        }

        if (settings.show_bounds != debug_settings.show_bounds)
            std::cout << "[Renderer] Bounding volumes " << (settings.show_bounds ? "shown" : "hidden") << ".\n";

        if (settings.print_stats != debug_settings.print_stats)
        {
            // The first print covers only frames measured from now on.
            statistics.Restart();

            std::cout << "[Renderer] Statistics " << (settings.print_stats ? "printed every second" : "off") << ".\n";
        }

        if (settings.isolate_gpu_timings != debug_settings.isolate_gpu_timings)
        {
            // Frames of the two modes are never averaged together.
            statistics.Restart();

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

    std::vector<Pipeline_Config> Renderer::Impl::Build_pipeline_manifest() const
    {
        return { opaque_config, transparent_config, bounds_config, composite_config };
    }

    // =========================================================
    // Renderer: public interface
    // =========================================================

    Renderer::Renderer(const Platform::Window& _window, Validation_Mode _validation_mode)
        : impl(std::make_unique<Impl>(_window, _validation_mode))
    {
    }

    Renderer::~Renderer()
    {
        impl.reset();

        std::cout << "[Renderer] Destroyed.\n";
    }

    uint32_t Renderer::Upload_mesh(const CoreTypes::MeshData& _mesh_data)
    {
        return impl->Upload_mesh(_mesh_data);
    }

    void Renderer::Release_mesh(uint32_t _gpu_id)
    {
        impl->Release_mesh(_gpu_id);
    }

    uint32_t Renderer::Upload_texture(const CoreTypes::ImageData& _image_data, CoreTypes::Pixel_Format _format)
    {
        return impl->Upload_texture(_image_data, _format);
    }

    uint32_t Renderer::Upload_texture(const CoreTypes::ImageData& _image_data)
    {
        return impl->Upload_texture(_image_data, _image_data.format);
    }

    Upload_Batch_Result Renderer::Upload_batch(const Upload_Batch& _batch)
    {
        return impl->Upload_batch(_batch);
    }

    uint32_t Renderer::Register_material(const Material_Desc& _desc)
    {
        return impl->Register_material(_desc);
    }

    uint32_t Renderer::Get_procedural_texture_index() const
    {
        return impl->procedural_texture_index;
    }

    uint8_t Renderer::Get_opaque_pipeline_id() const
    {
        return impl->opaque_pipeline_id;
    }

    uint8_t Renderer::Get_transparent_pipeline_id() const
    {
        return impl->transparent_pipeline_id;
    }

    const Render_Debug_Settings& Renderer::Get_debug_settings() const
    {
        return impl->debug_settings;
    }

    void Renderer::Set_debug_settings(const Render_Debug_Settings& _settings)
    {
        impl->Set_debug_settings(_settings);
    }

    void Renderer::Notify_framebuffer_resized()
    {
        impl->framebuffer_resized = true;
    }

    bool Renderer::Recreate_swapchain_if_needed()
    {
        return impl->Recreate_swapchain_if_needed();
    }

    void Renderer::Get_render_size(uint32_t& _out_width, uint32_t& _out_height) const
    {
        const VkExtent2D extent = impl->swapchain.Get_extent();
        _out_width = extent.width;
        _out_height = extent.height;
    }

    void Renderer::Render(const CoreTypes::RenderPacket& _packet)
    {
        impl->Render(_packet);
    }

} // namespace Renderer_System

