#include "Renderer_Impl.hpp"

#include <Cluster_Grid.hpp>
#include <Vulkan_Utils.hpp>

#include <MathConstants.hpp>
#include <Matrix4.hpp>
#include <Window.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

// The Renderer as an orchestrator: construction and destruction of the
// Vulkan stack and the subsystems, the frame loop (Render, a transaction
// that either submits the whole frame or undoes what it did), swapchain
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

        // CoreTypes does not include gpu_shared.h: the pass bits of
        // Object_GPU::flags are the macros the shaders read
        // (RENDER_PASS_* of scene_data.glsl), so the CoreTypes enum must
        // agree with them.
        static_assert(CoreTypes::Render_Pass_Bit::Opaque == GPU_RENDER_PASS_OPAQUE,
            "CoreTypes::Render_Pass_Bit::Opaque and RENDER_PASS_OPAQUE of the shaders must be the same bit");
        static_assert(CoreTypes::Render_Pass_Bit::Transparent == GPU_RENDER_PASS_TRANSPARENT,
            "CoreTypes::Render_Pass_Bit::Transparent and RENDER_PASS_TRANSPARENT of the shaders must be the same bit");

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

        // Size of the framebuffer of _window in pixels, which is the size
        // the swapchain is built for. Zero in a dimension while minimized.
        VkExtent2D Get_framebuffer_extent(const Platform::Window& _window)
        {
            int width = 0;
            int height = 0;
            _window.Get_framebuffer_size(width, height);

            return { static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0)) };
        }

        // Moves the frame slot on when it goes out of scope. Created once the
        // acquire succeeded, so the slot advances however the rest of the
        // frame ends: a normal frame, a frame that failed and was undone, or
        // one whose present failed after the submit.
        class Slot_Advance
        {
        public:

            Slot_Advance(uint32_t& _slot, uint32_t _slot_count) noexcept : slot(_slot), slot_count(_slot_count) {}
            ~Slot_Advance() { slot = (slot + 1) % slot_count; }

            Slot_Advance(const Slot_Advance&) = delete;
            Slot_Advance& operator=(const Slot_Advance&) = delete;

        private:

            uint32_t& slot;
            uint32_t  slot_count;
        };

        // True when vkQueuePresentKHR queued the present: the presentation
        // engine will wait for the semaphores and signal the present fence,
        // whatever the outcome. Out of memory (and a lost device) queue
        // nothing.
        bool Present_was_queued(VkResult _result)
        {
            return _result == VK_SUCCESS || _result == VK_SUBOPTIMAL_KHR ||
                   _result == VK_ERROR_OUT_OF_DATE_KHR || _result == VK_ERROR_SURFACE_LOST_KHR;
        }

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
        swapchain(device, surface, Get_framebuffer_extent(_window), 3, false),
        // The OIT formats are constants of Vulkan_OIT_Resources, which
        // creates the targets with exactly those. The depth format is the
        // floating-point one chosen with the device.
        render_pass(device, swapchain.Get_image_format(), device.Get_depth_format(),
                    Vulkan_OIT_Resources::ACCUMULATION_FORMAT, Vulkan_OIT_Resources::REVEALAGE_FORMAT),
        // The depth image must use exactly the format of the render pass.
        depth_resources(device, allocator.Get_handle(), render_pass.Get_depth_format(), swapchain.Get_extent()),
        oit_resources(device, allocator.Get_handle(), swapchain.Get_extent()),
        framebuffers(device, render_pass, swapchain, depth_resources, oit_resources),
        bindless_registry(device, BINDLESS_DESIRED_TEXTURES, BINDLESS_DESIRED_SAMPLERS),
        pipeline_cache(device),
        descriptor_layouts(device, bindless_registry.Get_layout()),
        pipeline_layout(device, descriptor_layouts),
        pipeline_registry(device, render_pass, pipeline_cache.Get_handle(), pipeline_layout.Get_handle()),
        compute_pipeline_layout(device, descriptor_layouts, Pipeline_Kind::Compute, VK_SHADER_STAGE_COMPUTE_BIT, COMPUTE_PUSH_CONSTANT_SIZE),
        procedural_pipeline(device, pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle(), "..\\..\\Renderer\\shaders\\compiled\\Procedural.comp.spv"),
        cull_pipeline(device, pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle(), "..\\..\\Renderer\\shaders\\compiled\\cull_objects.comp.spv"),
        light_clusters(device, allocator.Get_handle(), pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle()),
        opaque_config(Make_opaque_config()),
        transparent_config(Make_transparent_config()),
        bounds_config(Make_bounds_config(device.Is_fill_mode_non_solid_enabled())),
        composite_config(Make_composite_config()),
        sampler_cache(device),
        timeline(FRAMES_IN_FLIGHT),
        frame_semaphore(Create_timeline_semaphore(device.Get_logical_device_handle(), 0,
                                                  "Renderer: failed to create the frame timeline semaphore")),
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

            // Entry point to give back an image that was acquired and never
            // presented (swapchain maintenance1). The KHR name is tried
            // first; the EXT predecessor has the same signature.
            if (device.Is_swapchain_maintenance1_enabled())
            {
                const VkDevice dev = device.Get_logical_device_handle();

                release_swapchain_images = reinterpret_cast<PFN_vkReleaseSwapchainImagesKHR>(
                    vkGetDeviceProcAddr(dev, "vkReleaseSwapchainImagesKHR"));

                if (release_swapchain_images == nullptr)
                {
                    release_swapchain_images = reinterpret_cast<PFN_vkReleaseSwapchainImagesKHR>(
                        vkGetDeviceProcAddr(dev, "vkReleaseSwapchainImagesEXT"));
                }
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
    // Device loss
    // =========================================================

    void Renderer::Impl::Require_not_lost(const char* _operation) const
    {
        if (device_lost)
            throw std::runtime_error(std::string("Renderer: ") + _operation + " refused, the device is lost");
    }

    void Renderer::Impl::Note_current_failure() noexcept
    {
        try
        {
            throw;
        }
        catch (const Vulkan_Utils::Vulkan_Error& error)
        {
            if (error.Is_device_lost() && !device_lost)
            {
                device_lost = true;
                std::cerr << "[Renderer] The device was lost: " << error.what() << "\n";
            }
        }
        catch (...)
        {
            // Any other failure leaves the device usable.
        }
    }

    // =========================================================
    // Progress of the GPU
    // =========================================================

    void Renderer::Impl::Wait_for_serial(uint64_t _serial, const char* _what)
    {
        const VkDevice    dev = device.Get_logical_device_handle();
        const VkSemaphore semaphore = frame_semaphore.Get();

        if (!timeline.Is_complete(_serial))
        {
            VkSemaphoreWaitInfo wait_info{};
            wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait_info.semaphoreCount = 1;
            wait_info.pSemaphores = &semaphore;
            wait_info.pValues = &_serial;

            // A device loss ends the wait with an error instead of leaving it
            // pending, so this cannot hang on a dead device.
            VK_CHECK_SUCCESS(vkWaitSemaphores(dev, &wait_info, UINT64_MAX), _what);
        }

        // The counter says how far the GPU really got, which can be beyond
        // _serial: frames that finished in the meantime free their geometry
        // now instead of at the next wait that names them.
        uint64_t reached = 0;
        VK_CHECK(vkGetSemaphoreCounterValue(dev, semaphore, &reached), "Renderer: read the frame timeline semaphore");

        timeline.Mark_completed(reached);
    }

    // =========================================================
    // Surface size
    // =========================================================

    bool Renderer::Impl::Recreate_swapchain_if_needed()
    {
        if (!swapchain_recreation_pending) return true;

        // A minimized window has no framebuffer to build a swapchain for.
        // The flag stays set; the next call after restoring handles it.
        if (window.Is_minimized()) return false;

        return Recreate_swapchain();
    }

    // =========================================================
    // Render
    // =========================================================

    void Renderer::Impl::Render(const CoreTypes::RenderPacket& _packet)
    {
        // Nothing may wait on a device that will never answer.
        Require_not_lost("Render");

        try
        {
            Render_frame(_packet);
        }
        catch (...)
        {
            Note_current_failure();
            throw;
        }
    }

    void Renderer::Impl::Render_frame(const CoreTypes::RenderPacket& _packet)
    {
        VkDevice dev = device.Get_logical_device_handle();

        // A recreation the loop did not apply yet (or one that failed and is
        // being retried) is applied before the frame, so the frame is
        // rendered at the new size. While minimized, or while the swapchain
        // cannot be rebuilt, there is nothing to render into and nothing is
        // acquired. The loop calls Recreate_swapchain_if_needed before it
        // extracts the frame; this is the safety net for callers that do not.
        if (swapchain_recreation_pending && !Recreate_swapchain_if_needed()) return;

        Frame_Data& frame = frames[current_frame];

        // -- Wait for this frame slot to be free --
        // The serial of the slot's last submission: it returns at once for a
        // slot that never submitted, and for one whose last attempt failed
        // (a failed submit does not consume a serial). A device loss
        // surfaces here as an exception instead of the loop submitting
        // forever to a dead device.
        Wait_for_serial(timeline.Get_slot_serial(current_frame), "Render: wait for the frame slot");

        // -- Work of the previous use of this slot --
        // That frame and every earlier submission have completed. Geometry
        // released before then can go back to the pool, and the GPU results
        // of that frame can be read.
        mesh_registry.Free_completed(timeline);
        Read_frame_statistics(current_frame);

        // -- CPU work that does not depend on the swapchain image --
        // Before the acquire: an exception here holds no image, and the
        // image acquired below is held only for the recording and the
        // submit.
        Prepare_frame(frame, _packet);

        // -- Acquire swapchain image --
        uint32_t image_index = 0;
        const VkResult acquire_result = vkAcquireNextImageKHR(
            dev,
            swapchain.Get_handle(),
            UINT64_MAX,
            frame.image_available_semaphore.Get(),
            VK_NULL_HANDLE,
            &image_index
        );

        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Nothing was acquired and the semaphore was not signaled. The
            // loop recreates the swapchain before the next frame.
            swapchain_recreation_pending = true;
            return;
        }

        // VK_SUBOPTIMAL_KHR is a success: the image was acquired and the
        // semaphore will be signaled, so the frame must be rendered and
        // presented; the swapchain is recreated afterwards.
        VK_CHECK(acquire_result, "Render: failed to acquire swapchain image");

        // From here on an image belongs to this frame and the semaphore of
        // the slot has a signal pending. The slot advances however the frame
        // ends.
        const Slot_Advance advance_slot(current_frame, FRAMES_IN_FLIGHT);

        Present_Sync::Image_Sync& sync = present_sync.Get(image_index);

        Frame_Effects effects;

        // -- Protected region: from the acquire to the submit --
        // A frame that fails here never reaches the GPU. Its acquire is
        // undone, its effects are discarded (they are only applied below,
        // after the submit) and the exception goes on.
        try
        {
            // Wait for this image's previous present to complete.
            present_sync.Wait_for_previous_present(image_index);

            frame.command_pool.Reset_command_buffer(0);
            Record_command_buffer(frame, _packet, image_index, effects);

            Submit_frame(frame, sync.render_finished);
        }
        catch (...)
        {
            Recover_acquired_image(frame, image_index);
            throw;
        }

        // -- The frame is committed --
        // Its serial is what released geometry waits for, and what its
        // record is read back against.
        statistics.Mark_submitted(current_frame);

        // What the frame recorded is now on the GPU: the procedural image
        // stays valid for every later frame (End_write makes it visible to
        // all later work on the queue), and the cluster boxes hold the
        // projection they were rebuilt for. Applied here and not while
        // recording: a recording that threw or a failed submit leaves both
        // out of date, and the next frame records them again.
        Apply_effects(effects);

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
        }

        const VkResult present_result = vkQueuePresentKHR(device.Get_present_queue(), &present_info);

        // The fence is signaled only by a present that was queued. Marking
        // it pending before knowing, or after a failure that queued
        // nothing, would make the next wait for this image hang.
        if (sync.present_fence != VK_NULL_HANDLE && Present_was_queued(present_result))
            sync.present_pending = true;

        present_sync.Flush(false);

        // OUT_OF_DATE from present and SUBOPTIMAL from either call mean
        // the swapchain no longer matches the surface. The semaphore wait
        // of a rejected present is still executed by the queue, so the
        // retirement logic in Recreate_swapchain applies as usual. The
        // recreation itself is the loop's, before the next frame.
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR ||
            present_result == VK_SUBOPTIMAL_KHR ||
            acquire_result == VK_SUBOPTIMAL_KHR)
        {
            swapchain_recreation_pending = true;
        }
        else
        {
            // Out of memory queued nothing: render_finished stays signaled
            // and the acquired image was not presented. Recreating the
            // swapchain releases the image, and after the idle wait of that
            // recreation Present_Sync::Retire destroys the signaled
            // semaphore safely.
            if (present_result == VK_ERROR_OUT_OF_HOST_MEMORY || present_result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
                swapchain_recreation_pending = true;

            VK_CHECK(present_result, "Render: failed to present");
        }
    }

    // =========================================================
    // Prepare_frame
    // =========================================================

    void Renderer::Impl::Prepare_frame(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet)
    {
        draw_list.Update_culling_frustum(_packet.view.frustum, debug_settings.freeze_culling);

        // Culling is asked for by the path that names it. Where it runs is
        // the builder's decision: the culling pass when a GPU path draws the
        // opaque objects, the CPU otherwise (also after a fallback), and
        // always the CPU for the transparent items.
        Draw_List_Builder::Build_Settings settings;
        settings.requested_path = debug_settings.opaque_path;
        settings.frustum_culling = debug_settings.opaque_path == Opaque_Draw_Path::Gpu_Culled;
        settings.max_draw_indirect_count = device.Get_max_draw_indirect_count();

        draw_list.Build(_packet, static_cast<Object_GPU*>(_frame.object_buffer.mapped_ptr),
                        mesh_registry, pipeline_registry, material_table.Get_count(), settings);

        if (draw_list.Get_opaque_path() == Opaque_Draw_Path::Cpu_Indirect)
        {
            draw_list.Write_cpu_draw_commands(static_cast<VkDrawIndexedIndirectCommand*>(_frame.cpu_draw_command_buffer.mapped_ptr),
                                              mesh_registry);
            draw_list.Group_opaque_by_pipeline();
        }

        Write_frame_uniforms(_frame, _packet);
    }

    // =========================================================
    // Submit_frame
    // =========================================================

    void Renderer::Impl::Submit_frame(Frame_Data& _frame, VkSemaphore _render_finished)
    {
        // The serial this submission signals: the next one. It is consumed
        // only if the submit succeeds, so a failed one leaves the timeline as
        // it was and the next submission signals the same value.
        const uint64_t serial = timeline.Get_submitted_serial() + 1;

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkSemaphore          wait_semaphore = _frame.image_available_semaphore.Get();
        const VkCommandBuffer      command_buffer = _frame.Get_command_buffer();

        // The binary semaphores ignore their value; the timeline one takes
        // the serial.
        const uint64_t                    wait_value = 0;
        const std::array<VkSemaphore, 2>  signal_semaphores = { _render_finished, frame_semaphore.Get() };
        const std::array<uint64_t, 2>     signal_values = { 0, serial };

        VkTimelineSemaphoreSubmitInfo timeline_info{};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.waitSemaphoreValueCount = 1;
        timeline_info.pWaitSemaphoreValues = &wait_value;
        timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size());
        timeline_info.pSignalSemaphoreValues = signal_values.data();

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = &timeline_info;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &wait_semaphore;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        submit_info.signalSemaphoreCount = static_cast<uint32_t>(signal_semaphores.size());
        submit_info.pSignalSemaphores = signal_semaphores.data();

        VK_CHECK(vkQueueSubmit(device.Get_graphics_queue(), 1, &submit_info, VK_NULL_HANDLE),
            "Render: failed to submit command buffer");

        const uint64_t recorded = timeline.Record_submission(current_frame);
        assert(recorded == serial && "Submit_frame: the serial recorded is not the one that was signaled");
        (void)recorded;
    }

    // =========================================================
    // Apply_effects
    // =========================================================

    void Renderer::Impl::Apply_effects(const Frame_Effects& _effects)
    {
        if (_effects.procedural_recorded)
            procedural_dirty = false;

        if (_effects.cluster_boxes_recorded)
            light_clusters.Commit(_effects.cluster_projection, _effects.cluster_near_plane);
    }

    // =========================================================
    // Recover_acquired_image
    // =========================================================

    void Renderer::Impl::Recover_acquired_image(Frame_Data& _frame, uint32_t _image_index) noexcept
    {
        try
        {
            // -- The signal of the acquire --
            // The semaphore was signaled by the acquire and nothing waited
            // for it: acquiring with it again would be invalid usage. A
            // submission without commands consumes the signal, and signals
            // the next serial, so the wait for this slot in its next use
            // returns only once the semaphore is reusable. The frame's own
            // submission either failed (nothing was consumed) or never
            // happened, so the wait is still pending here.
            const uint64_t serial = timeline.Get_submitted_serial() + 1;

            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            const VkSemaphore          wait_semaphore = _frame.image_available_semaphore.Get();
            const uint64_t             wait_value = 0;
            const VkSemaphore          signal_semaphore = frame_semaphore.Get();

            VkTimelineSemaphoreSubmitInfo timeline_info{};
            timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timeline_info.waitSemaphoreValueCount = 1;
            timeline_info.pWaitSemaphoreValues = &wait_value;
            timeline_info.signalSemaphoreValueCount = 1;
            timeline_info.pSignalSemaphoreValues = &serial;

            VkSubmitInfo submit_info{};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.pNext = &timeline_info;
            submit_info.waitSemaphoreCount = 1;
            submit_info.pWaitSemaphores = &wait_semaphore;
            submit_info.pWaitDstStageMask = &wait_stage;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = &signal_semaphore;

            VK_CHECK(vkQueueSubmit(device.Get_graphics_queue(), 1, &submit_info, VK_NULL_HANDLE),
                "Recover_acquired_image: failed to consume the acquire semaphore");

            timeline.Record_submission(current_frame);
        }
        catch (...)
        {
            // Without it the semaphore cannot be reused and the slot's next
            // wait could not be trusted: nothing more can be done.
            try
            {
                std::cerr << "[Renderer] The recovery of a failed frame failed: the Renderer is lost.\n";
            }
            catch (...)
            {
            }

            device_lost = true;
            return;
        }

        // -- The image --
        // Given back to the swapchain when the extension allows it. Without
        // it, marking the swapchain for recreation is the way out: destroying
        // a swapchain frees the images that were acquired and never used.
        if (release_swapchain_images != nullptr)
        {
            VkReleaseSwapchainImagesInfoKHR release_info{};
            release_info.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_KHR;
            release_info.swapchain = swapchain.Get_handle();
            release_info.imageIndexCount = 1;
            release_info.pImageIndices = &_image_index;

            if (release_swapchain_images(device.Get_logical_device_handle(), &release_info) == VK_SUCCESS)
                return;
        }

        swapchain_recreation_pending = true;
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

        // The packet's array already is the std430 layout of the light
        // buffer (CoreTypes::GPU_Light) and the Extractor left the
        // directional lights first: mesh.frag loops over the first
        // directional_light_count entries, and the cluster pass only
        // distributes the rest. Cutting the array at MAX_LIGHTS therefore
        // drops local lights, never a sun.
        const uint32_t light_count = std::min<uint32_t>(packet_lights, MAX_LIGHTS);
        const uint32_t directional_count = std::min<uint32_t>(_packet.directional_light_count, light_count);

        if (light_count > 0)
        {
            std::memcpy(_frame.light_buffer.mapped_ptr, _packet.lights.data(),
                sizeof(CoreTypes::GPU_Light) * light_count);
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

        // -- Draw buckets --
        // Where each bucket of the opaque draws starts in the command buffer
        // and how many commands it holds; read by the culling pass. The
        // builder never produces more than MAX_DRAW_BUCKETS, and the rest of
        // the table stays zero.
        const std::vector<Draw_List_Builder::Draw_Bucket>& buckets = draw_list.Get_opaque_buckets();

        assert(buckets.size() <= MAX_DRAW_BUCKETS && "Write_frame_uniforms: more draw buckets than the uniform block holds");

        for (size_t i = 0; i < buckets.size(); ++i)
        {
            ubo.draw_buckets[i].first_command = buckets[i].first_command;
            ubo.draw_buckets[i].capacity = buckets[i].capacity;
        }

        std::memcpy(_frame.uniform_buffer.mapped_ptr, &ubo, sizeof(ubo));
    }

    // =========================================================
    // Recreate_swapchain
    // =========================================================

    bool Renderer::Impl::Recreate_swapchain()
    {
        // The recreation waits for the device to go idle.
        Require_not_lost("Recreate_swapchain");

        try
        {
            const VkExtent2D desired_extent = Get_framebuffer_extent(window);

            // A surface without area (a minimized window) cannot hold a
            // swapchain. Checked before anything is torn down: the idle wait
            // and the retirement of the presentation objects below would
            // otherwise leave a Renderer that cannot be brought back.
            if (!swapchain.Can_recreate(desired_extent))
                return false;

            VK_CHECK(vkDeviceWaitIdle(device.Get_logical_device_handle()),
                "Recreate_swapchain: wait for device idle");

            // Every submitted frame has completed: released geometry can go.
            timeline.Mark_all_complete();
            mesh_registry.Free_completed(timeline);

            // vkDeviceWaitIdle covers queue work, not presentation: the
            // per-image objects are retired through their present fences (or
            // a grace period without them), never reset while pending.
            present_sync.Retire();

            // The swapchain is replaced first, since everything else is
            // sized by it and built on its images. It cannot be undone: the
            // old one is retired by the creation. From here until the end,
            // the flag stays set, so the objects that still refer to the old
            // swapchain (the framebuffers) are never used, even if a step
            // below throws.
            if (!swapchain.Recreate(desired_extent))
                return false;

            // -- New objects, in temporaries --
            // Everything that can fail is created before any member is
            // replaced. If one of these throws, the temporaries release
            // themselves and the members are untouched.
            Vulkan_Depth_Resources new_depth(device, allocator.Get_handle(), device.Get_depth_format(), swapchain.Get_extent());
            Vulkan_OIT_Resources   new_oit(device, allocator.Get_handle(), swapchain.Get_extent());
            Vulkan_Framebuffer     new_framebuffers(device, render_pass, swapchain, new_depth, new_oit);

            // The image count may have changed: one Image_Sync per new image.
            // A throw here leaves a partial set, which the next Retire()
            // (the start of the next attempt) releases.
            present_sync.Create(swapchain.Get_image_count());

            // -- Replacement --
            // Moves cannot throw. The framebuffers go first: they refer to
            // the views of the other two.
            framebuffers = std::move(new_framebuffers);
            oit_resources = std::move(new_oit);
            depth_resources = std::move(new_depth);

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
            // procedural_dirty does for the procedural texture, and that
            // state is confirmed after the submit of the frame that rewrites
            // it (Frame_Effects), not here.

            swapchain_recreation_pending = false;
        }
        catch (...)
        {
            // The flag is still set: Render acquires nothing until a later
            // call completes the recreation. A lost device ends it for good.
            Note_current_failure();
            throw;
        }

        std::cout << "[Renderer] Swapchain recreated (" << swapchain.Get_image_count() << " images).\n";

        return true;
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
        impl->swapchain_recreation_pending = true;
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

    bool Renderer::Is_lost() const
    {
        return impl->device_lost;
    }

} // namespace Renderer_System

