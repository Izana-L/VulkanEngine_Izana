#include "Renderer_Impl.hpp"

#include <Vulkan_Utils.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

// The sequence of passes of a frame: command buffer recording and draw
// recording. The frame's CPU-side draw lists come from Draw_List_Builder
// (Renderer::Impl::Prepare_frame runs it before the recording), the compute
// passes of the clustered lighting from Light_Clusters.

namespace Renderer_System
{

    namespace
    {
        // The workgroup sizes of the compute passes (CULL_GROUP_SIZE,
        // PROCEDURAL_GROUP_SIZE) are in Renderer_Limits.hpp, built from the
        // same macros as the local_size of the shaders.

        // Stride of every indirect command buffer: tightly packed
        // VkDrawIndexedIndirectCommand, the layout cull_objects.comp writes.
        constexpr uint32_t DRAW_COMMAND_STRIDE = static_cast<uint32_t>(sizeof(VkDrawIndexedIndirectCommand));
    }

    // =========================================================
    // Record_command_buffer
    // =========================================================

    void Renderer::Impl::Record_command_buffer(Frame_Data& _frame, const RenderPacket& _packet,
                                               uint32_t _image_index, Frame_Effects& _out_effects)
    {
        const VkCommandBuffer command_buffer = _frame.Get_command_buffer();

        // -- CPU side of the frame --
        // Already done by Prepare_frame: the path that draws this frame's
        // opaque objects, the object buffer and the draw lists everything
        // below consumes. The transparent items are culled on the CPU when
        // culling is requested, with the same planes and the same bounding
        // volume test as the culling pass, so both discard the same objects.
        const Opaque_Draw_Path opaque_path = draw_list.Get_opaque_path();
        const bool             gpu_draws = Is_gpu_draw_path(opaque_path);
        const uint32_t         opaque_count = static_cast<uint32_t>(draw_list.Get_opaque_draws().size());

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info), "Record_command_buffer: begin");

        // Frame start point. Every block below is a Gpu_Scope: one name for
        // its debug label and its timed scope. The top-level scopes are all
        // opened outside the render pass, as the isolated timing mode
        // requires (it records a barrier before each of them).
        gpu_timer.Begin_frame(command_buffer, current_frame, debug_settings.isolate_gpu_timings);

        // -- Compute work: always before the render pass --
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

        // -- Procedural texture pass --
        // Writes every texel of procedural_image; the draws below sample it
        // through its bindless slot. Recorded only while its content is out
        // of date (procedural_dirty): once at startup today. Its barriers
        // serialize frames: Begin_write waits for the fragment and compute
        // work of everything submitted before, the previous frame included,
        // and End_write makes the compute passes below wait for the
        // dispatch. A frame without it lets the compute work of this frame
        // overlap the fragment work of the previous one.
        const bool record_procedural = procedural_dirty;

        if (record_procedural)
        {
            assert(procedural_image.has_value() && "Record_command_buffer: Init_procedural_pass() has not run");

            // Not up to date until the frame is submitted (Apply_effects).
            _out_effects.procedural_recorded = true;

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
            const uint32_t group_count_x = Dispatch_group_count(procedural_extent.width, PROCEDURAL_GROUP_SIZE);
            const uint32_t group_count_y = Dispatch_group_count(procedural_extent.height, PROCEDURAL_GROUP_SIZE);
            vkCmdDispatch(command_buffer, group_count_x, group_count_y, 1);

            // GENERAL -> SHADER_READ_ONLY_OPTIMAL, compute writes made
            // visible to the fragment stage before the render pass begins,
            // in this frame and in every later one.
            procedural_image->End_write(command_buffer);
        }

        // -- Transfer: cluster boxes and counter resets --
        // The boxes are rewritten only when the projection changed. The
        // atomic counters of both passes start every frame at zero; the
        // barrier after the fills also covers the box update, and reaches
        // every stage that reads what was reset: the atomics of the
        // compute passes, the indirect draw (draw count) and the
        // statistics copy. Its transfer write access also orders the
        // statistics copy of this frame after the copies earlier frames
        // made into the same readback buffer (write-after-write), with a
        // barrier instead of relying on the wait for the slot alone.
        {
            const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Uploads and resets", 0.5f, 0.5f, 0.5f);

            // The boxes count as rebuilt only after the submit
            // (Apply_effects): this frame may still fail, and then the
            // next one has to record the update again.
            if (light_clusters.Record_aabb_update(command_buffer, _packet.view.projection, _packet.view.near_plane))
            {
                _out_effects.cluster_boxes_recorded = true;
                _out_effects.cluster_projection = _packet.view.projection;
                _out_effects.cluster_near_plane = _packet.view.near_plane;
            }

            Vulkan_Buffer_Utils::Record_zero_fill_and_barrier(command_buffer,
                { { _frame.cluster_counter_buffer.buffer, 0, sizeof(Cluster_Counters_GPU) },
                  { _frame.gpu_draw_count_buffer.buffer, 0, sizeof(Draw_Count_GPU) } },
                { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT });
        }

        // -- Light assignment to clusters --
        {
            const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Light clusters", 1.0f, 0.9f, 0.2f);

            light_clusters.Record_dispatch(command_buffer, compute_pipeline_layout.Get_handle(),
                                           uploaded_directional_light_count, uploaded_light_count);
        }

        // -- Frustum culling and draw generation --
        // Only for the GPU paths; the opaque objects are entries
        // [0, opaque_count) of the object buffer, and each one carries the
        // bucket it is drawn in. A frame that does not record it simply has
        // no culling scope.
        if (gpu_draws && opaque_count > 0)
        {
            const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Frustum culling", 0.2f, 0.8f, 1.0f);

            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline.Get_handle());

            Cull_Push_Constants cull_push{};
            cull_push.object_count = opaque_count;
            cull_push.command_capacity = MAX_OBJECTS;
            cull_push.pass_bit = Render_Pass_Bit::Opaque;
            cull_push.frustum_culling = draw_list.Is_opaque_culled_on_gpu() ? 1u : 0u;

            vkCmdPushConstants(command_buffer, compute_pipeline_layout.Get_handle(), VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(Cull_Push_Constants), &cull_push);

            vkCmdDispatch(command_buffer, Dispatch_group_count(opaque_count, CULL_GROUP_SIZE), 1, 1);
        }

        // -- Compute results -> consumers --
        // One barrier for both passes: the cluster lists to the fragment
        // shader, the draw commands and their count to the indirect draw,
        // and the counters to the statistics copy after the render pass.
        // Forgetting the indirect part works "almost always", which is why
        // it is spelled out. Outside every scope: its cost shows as "other"
        // in the timings.
        Vulkan_Buffer_Utils::Record_compute_to_consumer_barrier(command_buffer,
            { VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT });

        // -- Render pass --
        // Its scope holds the pass alone: the statistics copies, which only
        // depend on the barrier above, are recorded after it.
        Draw_State draw_state;
        draw_state.front_face = raster_state.front_face;

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

            // -- Dynamic viewport + scissor --
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

            // -- Dynamic raster + depth state --
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
            // It is the state for objects whose transform keeps the winding;
            // the mirrored ones (negative determinant) invert it, and the
            // draws set the opposite front face for them
            // (Set_front_face).
            vkCmdSetCullMode(command_buffer, raster_state.cull_mode);
            vkCmdSetFrontFace(command_buffer, raster_state.front_face);
            vkCmdSetDepthTestEnable(command_buffer, raster_state.depth_test_enable ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthWriteEnable(command_buffer, raster_state.depth_write_enable ? VK_TRUE : VK_FALSE);
            vkCmdSetDepthCompareOp(command_buffer, raster_state.depth_compare_op);

            // -- Bind descriptor sets --
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

            // -- Geometry: one bind for the whole frame --
            // Every mesh lives in the pool; the draws select theirs through
            // firstIndex / vertexOffset. The composite pipeline declares no
            // vertex input and ignores it.
            geometry_pool.Bind(command_buffer);

            // -- Subpass 0: opaque, depth write on --
            {
                const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Opaque", 0.3f, 0.9f, 0.3f);
                Record_opaque_draws(command_buffer, _frame, draw_state);
            }

            // -- Subpass 1: transparent accumulation --
            // The depth buffer is read-only from here on (its layout in
            // subpasses 1 and 2): every draw must have depth writes
            // disabled. The transparent draws are order-independent: the
            // list needs no back-to-front order.
            vkCmdNextSubpass(command_buffer, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetDepthWriteEnable(command_buffer, VK_FALSE);

            {
                const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Transparent", 0.3f, 0.5f, 1.0f);
                Record_transparent_draws(command_buffer, draw_state);
            }

            // -- Subpass 2: composite, then debug views --
            vkCmdNextSubpass(command_buffer, VK_SUBPASS_CONTENTS_INLINE);

            // A frame without transparent draws left the targets at their
            // clear values, whose composite changes nothing: skipped.
            if (!draw_list.Get_transparent_draws().empty())
            {
                const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "OIT composite", 0.6f, 0.4f, 1.0f);
                Record_composite_draw(command_buffer, draw_state);
            }

            // Behind the composite: the volumes are drawn over the final
            // color and tested against the opaque depth.
            if (debug_settings.show_bounds)
            {
                const Gpu_Scope scope(debug_utils, gpu_timer, command_buffer, "Bounding volumes", 1.0f, 1.0f, 1.0f);
                Record_bounds_draw(command_buffer, draw_state);
            }

            vkCmdEndRenderPass(command_buffer);
        }

        if (draw_state.bind_count != last_reported_binds)
        {
            std::cout << "[Renderer] " << draw_state.bind_count << " pipeline bind(s) for "
                      << _packet.opaque_items.size() << " opaque + "
                      << _packet.transparent_items.size() << " transparent item(s).\n";
            last_reported_binds = draw_state.bind_count;
        }

        // -- Statistics readback --
        // The counters of this frame are copied into its host-visible
        // readback buffer, read when this frame slot comes around again
        // (Read_frame_statistics), so the CPU never waits for them. They
        // depend only on the compute -> consumers barrier, so they follow
        // the render pass instead of sitting inside its scope. A label
        // only, not a timed scope: its cost shows as "other".
        {
            const Debug_Label_Scope label(debug_utils, command_buffer, "Statistics readback", 0.5f, 0.5f, 0.5f);

            Frame_Statistics::Record_readback(command_buffer, _frame.cluster_counter_buffer,
                                              _frame.gpu_draw_count_buffer, _frame.stats_readback_buffer);
        }

        // Frame end point, after every command of the frame.
        gpu_timer.End_frame(command_buffer);

        // The driver reports recording errors here, not at the individual
        // vkCmd* calls.
        VK_CHECK(vkEndCommandBuffer(command_buffer), "Record_command_buffer: end");

        // Context of this frame's counters, read back with them. Marked as
        // submitted by Render once the submit succeeded.
        Frame_Statistics::Frame_Record record;
        record.opaque_path = opaque_path;
        record.opaque_candidates = draw_list.Get_opaque_candidates();
        record.opaque_objects = opaque_count;
        record.transparent_candidates = draw_list.Get_transparent_candidates();
        record.transparent_drawn = static_cast<uint32_t>(draw_list.Get_transparent_draws().size());

        statistics.Stage_record(current_frame, record);
    }

    // =========================================================
    // Draw recording
    // =========================================================

    void Renderer::Impl::Bind_graphics_pipeline(VkCommandBuffer _command_buffer, uint8_t _pipeline_id, Draw_State& _state)
    {
        if (_pipeline_id == _state.bound_pipeline_id)
            return;

        // Every id reaching here was validated by Draw_List_Builder, or is
        // one of the Renderer's own pipelines.
        vkCmdBindPipeline(_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_registry.Get_by_id(_pipeline_id));
        _state.bound_pipeline_id = _pipeline_id;
        ++_state.bind_count;
    }

    void Renderer::Impl::Set_front_face(VkCommandBuffer _command_buffer, bool _mirrored, Draw_State& _state)
    {
        // An object whose transform has a negative determinant inverts the
        // winding of its triangles, so its front faces are the ones wound
        // the other way.
        const VkFrontFace wanted = _mirrored
            ? (raster_state.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE)
            : raster_state.front_face;

        if (wanted == _state.front_face)
            return;

        vkCmdSetFrontFace(_command_buffer, wanted);
        _state.front_face = wanted;
    }

    void Renderer::Impl::Record_opaque_draws(VkCommandBuffer _command_buffer, Frame_Data& _frame, Draw_State& _state)
    {
        const std::vector<Draw_List_Builder::Draw_Record>& opaque_draws = draw_list.Get_opaque_draws();

        if (opaque_draws.empty())
            return;

        switch (draw_list.Get_opaque_path())
        {
        case Opaque_Draw_Path::Direct:
        {
            // One draw per object, straight from the pool. The winding is
            // set only when it changes from one object to the next.
            for (const Draw_List_Builder::Draw_Record& draw : opaque_draws)
            {
                Bind_graphics_pipeline(_command_buffer, draw.pipeline_id, _state);
                Set_front_face(_command_buffer, draw.mirrored, _state);
                mesh_registry.Get(draw.mesh_id).Draw(_command_buffer, draw.object_index);
            }
            break;
        }

        case Opaque_Draw_Path::Cpu_Indirect:
        {
            // One vkCmdDrawIndexedIndirect per run of commands sharing a
            // pipeline and a winding, split at maxDrawIndirectCount.
            const size_t max_per_call = std::max<uint32_t>(1u, device.Get_max_draw_indirect_count());

            for (const Draw_List_Builder::Pipeline_Run& run : draw_list.Get_opaque_runs())
            {
                Bind_graphics_pipeline(_command_buffer, run.pipeline_id, _state);
                Set_front_face(_command_buffer, run.mirrored, _state);

                const size_t run_end = static_cast<size_t>(run.first) + run.count;

                for (size_t first = run.first; first < run_end; first += max_per_call)
                {
                    const uint32_t draw_count = static_cast<uint32_t>(std::min(max_per_call, run_end - first));

                    vkCmdDrawIndexedIndirect(_command_buffer, _frame.cpu_draw_command_buffer.buffer,
                        static_cast<VkDeviceSize>(first) * DRAW_COMMAND_STRIDE, draw_count, DRAW_COMMAND_STRIDE);
                }
            }
            break;
        }

        case Opaque_Draw_Path::Gpu_Indirect:
        case Opaque_Draw_Path::Gpu_Culled:
        {
            // One indirect draw per bucket: the objects of one pipeline and
            // one winding. The culling pass wrote as many commands into the
            // region of the bucket as it kept, and counted them in the
            // counter of the bucket; the capacity of the region is an upper
            // bound, not the number drawn.
            const std::vector<Draw_List_Builder::Draw_Bucket>& buckets = draw_list.Get_opaque_buckets();

            for (size_t bucket_index = 0; bucket_index < buckets.size(); ++bucket_index)
            {
                const Draw_List_Builder::Draw_Bucket& bucket = buckets[bucket_index];

                Bind_graphics_pipeline(_command_buffer, bucket.pipeline_id, _state);
                Set_front_face(_command_buffer, bucket.mirrored, _state);

                vkCmdDrawIndexedIndirectCount(_command_buffer,
                    _frame.gpu_draw_command_buffer.buffer, static_cast<VkDeviceSize>(bucket.first_command) * DRAW_COMMAND_STRIDE,
                    _frame.gpu_draw_count_buffer.buffer,
                    offsetof(Draw_Count_GPU, bucket_draw_count) + bucket_index * sizeof(uint32_t),
                    bucket.capacity, DRAW_COMMAND_STRIDE);
            }
            break;
        }

        default:
            break;
        }
    }

    void Renderer::Impl::Record_transparent_draws(VkCommandBuffer _command_buffer, Draw_State& _state)
    {
        const std::vector<Draw_List_Builder::Draw_Record>& transparent_draws = draw_list.Get_transparent_draws();

        if (transparent_draws.empty())
            return;

        // Weighted blended OIT: every fragment is accumulated into the two
        // targets, so the result does not depend on the draw order and the
        // list needs no back-to-front sort. It arrives grouped by pipeline,
        // material and mesh, like the opaque one, which keeps the binds to
        // a minimum. The opaque depth occludes the fragments; depth writes
        // were disabled when the subpass began. The winding is set only
        // when it changes, like the pipeline.
        for (const Draw_List_Builder::Draw_Record& draw : transparent_draws)
        {
            Bind_graphics_pipeline(_command_buffer, draw.pipeline_id, _state);
            Set_front_face(_command_buffer, draw.mirrored, _state);
            mesh_registry.Get(draw.mesh_id).Draw(_command_buffer, draw.object_index);
        }
    }

    void Renderer::Impl::Record_composite_draw(VkCommandBuffer _command_buffer, Draw_State& _state)
    {
        Bind_graphics_pipeline(_command_buffer, composite_pipeline_id, _state);

        // Every pixel is resolved, whatever the opaque depth holds, and the
        // full-screen triangle is not culled by its winding.
        vkCmdSetDepthTestEnable(_command_buffer, VK_FALSE);
        vkCmdSetCullMode(_command_buffer, VK_CULL_MODE_NONE);

        // Three vertices generated from gl_VertexIndex (oit_composite.vert);
        // the targets are read through set 1 as input attachments.
        vkCmdDraw(_command_buffer, 3, 1, 0, 0);
    }

    void Renderer::Impl::Record_bounds_draw(VkCommandBuffer _command_buffer, Draw_State& _state)
    {
        // Entries [0, object_count) of the object buffer: the opaque objects
        // and the transparent ones that survived the CPU culling. Opaque
        // objects culled on the GPU keep their volume, which is what shows
        // the culling at work.
        const uint32_t object_count = static_cast<uint32_t>(draw_list.Get_opaque_draws().size() + draw_list.Get_transparent_draws().size());

        if (object_count == 0 || bounds_sphere_mesh_id >= mesh_registry.Get_count())
            return;

        const Mesh_GPU& sphere = mesh_registry.Get(bounds_sphere_mesh_id);

        if (sphere.released || !sphere.geometry.Is_valid())
            return;

        Bind_graphics_pipeline(_command_buffer, bounds_pipeline_id, _state);

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

} // namespace Renderer_System
