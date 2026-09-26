#include <Renderer.hpp>
#include <Vulkan_Utils.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Window.hpp>

#include <glm/glm.hpp>

#include <stdexcept>
#include <iostream>
#include <cstring>
#include <array>
#include <cassert>
#include <string>

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
    }

    // =========================================================
    // Pipeline configurations
    // =========================================================

    Pipeline_Config Renderer::Make_opaque_config()
    {
        Pipeline_Config config;
        config.vertex_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.vert.spv";
        config.fragment_shader_path = "..\\..\\Renderer\\shaders\\compiled\\mesh.frag.spv";
        config.blend_enable = false;
        return config;
    }

    Pipeline_Config Renderer::Make_transparent_config()
    {
        // Same shaders; standard "over" alpha blending. Depth writes are
        // disabled at draw time through dynamic state, not here.
        Pipeline_Config config = Make_opaque_config();
        config.blend_enable = true;
        config.src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA;
        config.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        config.color_blend_op = VK_BLEND_OP_ADD;
        config.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        config.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        config.alpha_blend_op = VK_BLEND_OP_ADD;
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
                        swapchain(device, surface, _window, 3, false),
                        render_pass(device, swapchain.Get_image_format(), device.Find_supported_depth_format()),
                        // The depth format is negotiated once, by the render pass; the
                        // depth image must use exactly that one.
                        depth_resources(device, allocator.Get_handle(), render_pass.Get_depth_format(), swapchain.Get_extent()),
                        framebuffers(device, render_pass, swapchain, depth_resources),
                        bindless_registry(device, BINDLESS_DESIRED_TEXTURES, BINDLESS_DESIRED_SAMPLERS),
                        pipeline_cache(device),
                        descriptor_layouts(device, bindless_registry.Get_layout()),
                        pipeline_layout(device, descriptor_layouts),
                        pipeline_registry(device, render_pass, pipeline_cache.Get_handle(), pipeline_layout.Get_handle()),
                        compute_pipeline_layout(device, descriptor_layouts, VK_SHADER_STAGE_COMPUTE_BIT, static_cast<uint32_t>(sizeof(Procedural_Push_Constants))),
                        procedural_pipeline(device, pipeline_cache.Get_handle(), compute_pipeline_layout.Get_handle(),"..\\..\\Renderer\\shaders\\compiled\\procedural.comp.spv"),
                        opaque_config(Make_opaque_config()),
                        transparent_config(Make_transparent_config()),
                        sampler_cache(device),
                        transfer_command_pool(device, 0, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT)
    {
        try
        {
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

            // Pure lookups: both pipelines already exist after the warm-up.
            opaque_pipeline_id = pipeline_registry.Get_id(opaque_config);
            transparent_pipeline_id = pipeline_registry.Get_id(transparent_config);

            // ── Frame resources ────────────────────────────────────────
            frames.reserve(FRAMES_IN_FLIGHT);
            for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
                frames.emplace_back(device, allocator.Get_handle());

            Create_image_sync();

            // ── Descriptor pool + sets ─────────────────────────────────
            Init_descriptor_pool();
            Init_descriptor_sets();

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
        // referenced by in-flight command buffers otherwise.
        meshes.clear();
        textures.clear();

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
        }

        for (const Texture_Upload& upload : _batch.textures)
        {
            if (upload.data == nullptr)
                throw std::invalid_argument("Upload_batch: null ImageData pointer");
            if (upload.data->pixels.empty() || upload.data->width == 0 || upload.data->height == 0)
                throw std::invalid_argument("Upload_batch: ImageData has no pixel data or zero dimensions");
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
        // mid-batch and move every Mesh_GPU / Texture_GPU already recorded.
        // Those moves are safe (both null out the source), but reserving
        // avoids the churn and keeps the registries stable while we build.
        meshes.reserve(meshes.size() + mesh_count);
        textures.reserve(textures.size() + texture_count);

        result.mesh_gpu_ids.reserve(mesh_count);
        result.texture_bindless_indices.reserve(texture_count);

        // Where this batch starts, so the post-submit pass below only
        // touches what this call added, and the failure path can undo it.
        const size_t first_mesh = meshes.size();
        const size_t first_texture = textures.size();

        // ── One command buffer for the whole batch ────────────────
        VkCommandBuffer transfer_cmd = transfer_command_pool.Allocate_primary();

        try
        {
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(transfer_cmd, &begin_info), "Upload_batch: begin transfer command buffer");

            // ── Record every asset into that one command buffer ───────
            // No barriers are needed BETWEEN assets: each Mesh_GPU writes its
            // own buffers and each Texture_GPU barriers its own image, so the
            // recordings touch disjoint resources. The barriers that do exist
            // (layout transitions, mip generation) are internal to each
            // Texture_GPU and already correct.
            for (const CoreTypes::MeshData* mesh_data : _batch.meshes)
            {
                meshes.emplace_back(allocator.Get_handle(), transfer_cmd, *mesh_data);
                result.mesh_gpu_ids.push_back(static_cast<uint32_t>(meshes.size() - 1));
            }

            for (const Texture_Upload& upload : _batch.textures)
            {
                textures.emplace_back(device, allocator.Get_handle(), transfer_cmd, *upload.data, upload.format);
            }

            // ── End, submit, wait: ONCE for the whole batch ──────────
            Submit_and_wait_transfer(transfer_cmd);
        }
        catch (...)
        {
            // Nothing of this batch reached the GPU in a usable state:
            // drop the registry entries added above (their destructors
            // free the buffers; the transfer either never ran or was
            // waited for by Submit_and_wait_transfer before it threw) and
            // give the command buffer back.
            vkDeviceWaitIdle(device.Get_logical_device_handle());
            meshes.erase(meshes.begin() + static_cast<std::ptrdiff_t>(first_mesh), meshes.end());
            textures.erase(textures.begin() + static_cast<std::ptrdiff_t>(first_texture), textures.end());
            transfer_command_pool.Free(transfer_cmd);
            throw;
        }

        transfer_command_pool.Free(transfer_cmd);

        // ── Post-upload ───────────────────────────────────────────
        // The fence is signaled, so the GPU has consumed every staging
        // buffer in the batch and they can all be freed now.

        for (size_t i = first_mesh; i < meshes.size(); ++i)
            meshes[i].Release_staging_buffers();

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
            << mesh_count << " mesh(es), "
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
        const uint32_t light_count = (packet_lights > MAX_LIGHTS) ? MAX_LIGHTS : packet_lights;
        ubo.light_count = static_cast<int32_t>(light_count);

        // Reported when the overflow starts and whenever the light count
        // changes while it lasts, so a scene that stays over the limit
        // does not print on every frame.
        if (packet_lights > MAX_LIGHTS)
        {
            if (packet_lights != reported_light_overflow)
            {
                std::cerr << "[Renderer] " << packet_lights << " lights in the packet, only the first "
                    << MAX_LIGHTS << " are uploaded.\n";
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
        Light_GPU* gpu_lights = static_cast<Light_GPU*>(_frame.light_buffer.mapped_ptr);

        for (uint32_t i = 0; i < light_count; ++i)
        {
            const CoreTypes::GPU_Light& src = _packet.lights[i];
            Light_GPU& dst = gpu_lights[i];

            dst.position_or_direction = src.position_or_direction;
            dst.intensity = src.intensity;
            dst.color = src.color;
            dst.range = src.range;
            dst.spot_direction = src.spot_direction;
            dst.inner_angle = src.inner_angle;
            dst.outer_angle = src.outer_angle;
            dst.type = static_cast<int32_t>(src.type);
            dst._padding0 = 0.0f;
            dst._padding1 = 0.0f;
        }

        std::memcpy(_frame.uniform_buffer.mapped_ptr, &ubo, sizeof(ubo));
    }

    // =========================================================
    // Record_command_buffer
    // =========================================================

    void Renderer::Record_command_buffer(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet, uint32_t _image_index)
    {
        const VkCommandBuffer command_buffer = _frame.Get_command_buffer();

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info), "Record_command_buffer: begin");

        // ── Compute work: always before the render pass ───────────
        // Every compute pass that writes an image registered in the
        // bindless set is recorded here, with its two barriers
        // (Storage_Image::Begin_write / End_write), before
        // vkCmdBeginRenderPass:
        //   - a pipeline barrier inside the render pass requires a
        //     subpass self-dependency, and the render pass declares none
        //     (Vulkan_Render_Pass only declares EXTERNAL -> 0);
        //   - End_write must execute before any draw that may read the
        //     bindless set (declared layout rule,
        //     Bindless_Registry::Register_texture).
                // Compute passes bind their pipeline and descriptor sets at
        // VK_PIPELINE_BIND_POINT_COMPUTE: the sets bound below for
        // GRAPHICS are not visible to dispatches, and binding compute sets
        // does not disturb them.

        // ── Procedural texture pass (milestone 1.1: empty dispatch) ─
        // Fixed size, independent of the swapchain: no recreation on
        // resize. Replaced by the extent of the target Storage_Image once
        // the pass writes one (milestone 1.2).
        constexpr uint32_t PROCEDURAL_SIZE = 256;
        constexpr uint32_t PROCEDURAL_GROUP_SIZE = 8;   // local_size_x / local_size_y of procedural.comp
        {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, procedural_pipeline.Get_handle());

            // Same set handles as the graphics pass: compute_pipeline_layout
            // uses the same set layouts, so they are compatible. Set 1 is
            // left unbound until the pass declares its storage image.
            const VkDescriptorSet compute_per_frame_set = descriptor_sets[current_frame];
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline_layout.Get_handle(),
                Descriptor_Set::Per_Frame, 1, &compute_per_frame_set, 0, nullptr);

            const VkDescriptorSet compute_bindless_set = bindless_registry.Get_set();
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline_layout.Get_handle(),
                Descriptor_Set::Bindless, 1, &compute_bindless_set, 0, nullptr);

            Procedural_Push_Constants procedural_push{};
            procedural_push.image_width = PROCEDURAL_SIZE;
            procedural_push.image_height = PROCEDURAL_SIZE;
            procedural_push.time = 0.0f;   // animated in milestone 1.3

            // Stage flags must match the range of compute_pipeline_layout
            // exactly (VK_SHADER_STAGE_COMPUTE_BIT).
            vkCmdPushConstants(command_buffer, compute_pipeline_layout.Get_handle(), VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(Procedural_Push_Constants), &procedural_push);

            // Rounded up: a size that is not a multiple of the group size
            // still covers every texel; the shader discards the excess.
            const uint32_t group_count_x = (PROCEDURAL_SIZE + PROCEDURAL_GROUP_SIZE - 1) / PROCEDURAL_GROUP_SIZE;
            const uint32_t group_count_y = (PROCEDURAL_SIZE + PROCEDURAL_GROUP_SIZE - 1) / PROCEDURAL_GROUP_SIZE;
            vkCmdDispatch(command_buffer, group_count_x, group_count_y, 1);
        }

        // ── Render pass ───────────────────────────────────────────
        std::array<VkClearValue, 2> clear_values{};
        clear_values[0].color = { { _packet.clear_color.r, _packet.clear_color.g, _packet.clear_color.b, _packet.clear_color.a } };

        // Reverse-Z: 0.0 is the far end, so that is what "nothing drawn yet"
        // means. Leave this at 1.0 and every fragment fails the GREATER test
        // - black screen, no validation error, nothing to debug.
        clear_values[1].depthStencil = { 0.0f, 0 };

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
        // MUST be set before any draw in this command buffer.
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
        // Set 0: per-frame view/projection UBO + light buffer.
        // Set 3: global bindless texture array, bound once here and
        // indexed by every draw item through its push constants.
        VkDescriptorSet per_frame_set = descriptor_sets[current_frame];
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.Get_handle(),
                                Descriptor_Set::Per_Frame, 1, &per_frame_set, 0, nullptr);

        VkDescriptorSet bindless_set = bindless_registry.Get_set();
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.Get_handle(),
                                Descriptor_Set::Bindless, 1, &bindless_set, 0, nullptr);

        uint8_t  bound_pipeline_id = 0xFF;
        uint32_t bind_count = 0;

        // ── Opaque pass: depth write on ───────────────────────────
        Draw_items(command_buffer, _packet.opaque_items, CoreTypes::Render_Pass_Bit::Opaque, _packet, bound_pipeline_id, bind_count);

        // ── Transparent pass: depth test only, back-to-front ──────
        // The list arrives sorted back-to-front by the extract; blending
        // needs the opaque depth to occlude but must not write its own.
        if (!_packet.transparent_items.empty())
        {
            vkCmdSetDepthWriteEnable(command_buffer, VK_FALSE);

            Draw_items(command_buffer, _packet.transparent_items, CoreTypes::Render_Pass_Bit::Transparent, _packet, bound_pipeline_id, bind_count);
        }

        if (bind_count != last_reported_binds)
        {
            std::cout << "[Renderer] " << bind_count << " pipeline bind(s) for "
                      << _packet.opaque_items.size() << " opaque + "
                      << _packet.transparent_items.size() << " transparent item(s).\n";
            last_reported_binds = bind_count;
        }

        vkCmdEndRenderPass(command_buffer);

        // The driver reports recording errors here, not at the individual
        // vkCmd* calls.
        VK_CHECK(vkEndCommandBuffer(command_buffer), "Record_command_buffer: end");
    }

    // =========================================================
    // Draw_items
    // =========================================================

    void Renderer::Draw_items(VkCommandBuffer _command_buffer,
        const std::vector<CoreTypes::Draw_Item>& _items,
        uint8_t _pass_bit,
        const CoreTypes::RenderPacket& _packet,
        uint8_t& _bound_pipeline_id,
        uint32_t& _bind_count)
    {
        for (const CoreTypes::Draw_Item& item : _items)
        {
            // An item that does not take part in this pass is skipped, so a
            // pass_mask actually selects passes instead of being decoration.
            if ((item.pass_mask & _pass_bit) == 0) continue;

            const uint8_t item_pipeline_id = CoreTypes::Get_pipeline_id(item.sort_key);
            const VkPipeline pipeline = pipeline_registry.Get_by_id(item_pipeline_id);

            // Every reference is validated in every build: an out-of-range
            // transform index would read past the packet's array.
            const bool valid =
                pipeline != VK_NULL_HANDLE &&
                item.mesh_gpu_id < meshes.size() &&
                item.transform_idx < _packet.transform_count;

            if (!valid)
            {
                if (!warned_invalid_item)
                {
                    std::cerr << "[Renderer] Draw item skipped: pipeline " << int(item_pipeline_id)
                        << ", mesh " << item.mesh_gpu_id << ", transform " << item.transform_idx
                        << " (registered meshes: " << meshes.size() << ", transforms in packet: "
                        << _packet.transform_count << "). Further occurrences are not reported.\n";
                    warned_invalid_item = true;
                }
                continue;
            }

            if (item_pipeline_id != _bound_pipeline_id)
            {
                vkCmdBindPipeline(_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                _bound_pipeline_id = item_pipeline_id;
                ++_bind_count;
            }

            // Per-draw data: model matrix (vertex stage), tint, texture
// index and sampler index (fragment stage), all in one push.
            Push_Constants push{};
            push.model = _packet.transforms[item.transform_idx];
            push.base_color = item.base_color;
            push.albedo_texture_index = item.albedo_texture_index;
            push.albedo_sampler_index = item.albedo_sampler_index;

            // An index that was never registered reads an unwritten
            // descriptor, which PARTIALLY_BOUND turns into undefined
            // behaviour rather than a validation error, and without GPU-AV
            // nothing reports it. Debug builds stop at the assert; every
            // build replaces the index with a valid fallback before the
            // push, so a bad index never reaches the shader: the error
            // texture for a texture, the default preset for a sampler.
            const bool texture_valid = bindless_registry.Is_texture_registered(push.albedo_texture_index);
            const bool sampler_valid = push.albedo_sampler_index < static_cast<uint32_t>(CoreTypes::Sampler_Preset::Count);

            assert(texture_valid && "Draw_Item::albedo_texture_index is not a registered bindless texture slot");
            assert(sampler_valid && "Draw_Item::albedo_sampler_index is not a Sampler_Preset value");

            if (!texture_valid || !sampler_valid)
            {
                if (!warned_invalid_material_index)
                {
                    std::cerr << "[Renderer] Draw item with texture index " << push.albedo_texture_index
                        << " and sampler index " << push.albedo_sampler_index << " drawn with "
                        << (texture_valid ? "its texture" : "the error texture") << " and "
                        << (sampler_valid ? "its sampler" : "the default sampler")
                        << ". Further occurrences are not reported.\n";
                    warned_invalid_material_index = true;
                }

                if (!texture_valid)
                    push.albedo_texture_index = CoreTypes::Default_Texture::Error;

                if (!sampler_valid)
                    push.albedo_sampler_index = static_cast<uint32_t>(CoreTypes::Sampler_Preset::Linear_Repeat);
            }

            vkCmdPushConstants(_command_buffer, pipeline_layout.Get_handle(),
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(Push_Constants), &push);

            const Mesh_GPU& mesh = meshes[item.mesh_gpu_id];
            mesh.Bind(_command_buffer);
            mesh.Draw(_command_buffer);
        }
    }

    // =========================================================
    // Recreate_swapchain
    // =========================================================

    void Renderer::Recreate_swapchain()
    {
        VK_CHECK(vkDeviceWaitIdle(device.Get_logical_device_handle()),
            "Recreate_swapchain: wait for device idle");

        // vkDeviceWaitIdle covers queue work, not presentation: the
        // per-image objects are retired through their present fences (or
        // a grace period without them), never reset while pending.
        Retire_image_sync();

        swapchain.Recreate();
        depth_resources.Recreate(swapchain.Get_extent());
        framebuffers.Recreate(render_pass, swapchain, depth_resources);

        // Resolution-dependent images registered in the bindless set (the
        // Storage_Image outputs of compute passes) belong here as well,
        // after the idle wait above: Storage_Image::Recreate, with its
        // clear submitted before the next frame reads the slot, then
        // Bindless_Registry::Update_texture on the slot the image already
        // holds. Every draw keeps its index and reads a live view, and no
        // slot is consumed per resize.

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
        // One uniform buffer and one storage buffer descriptor per frame-in-flight.
        std::array<VkDescriptorPoolSize, 2> pool_sizes{};

        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_sizes[0].descriptorCount = FRAMES_IN_FLIGHT;

        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[1].descriptorCount = FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        pool_info.maxSets = FRAMES_IN_FLIGHT;   // one set 0 per frame slot

        VK_CHECK(vkCreateDescriptorPool(device.Get_logical_device_handle(), &pool_info, nullptr, &descriptor_pool),
            "Renderer: failed to create descriptor pool");
    }

    // =========================================================
    // Init_descriptor_sets
    // =========================================================

    void Renderer::Init_descriptor_sets()
    {
        std::array<VkDescriptorSetLayout, FRAMES_IN_FLIGHT> layouts;
        layouts.fill(descriptor_layouts.Get(Descriptor_Set::Per_Frame));

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

            std::array<VkWriteDescriptorSet, 2> writes{};

            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = descriptor_sets[i];
            writes[0].dstBinding = Binding_Per_Frame::Frame_UBO;
            writes[0].dstArrayElement = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &ubo_info;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = descriptor_sets[i];
            writes[1].dstBinding = Binding_Per_Frame::Lights;
            writes[1].dstArrayElement = 0;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo = &light_info;

            vkUpdateDescriptorSets(device.Get_logical_device_handle(),
                static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    // =========================================================
    // Build_pipeline_manifest
    // =========================================================

    std::vector<Pipeline_Config> Renderer::Build_pipeline_manifest() const
    {
        return { opaque_config, transparent_config };
    }

} // namespace Renderer_System
