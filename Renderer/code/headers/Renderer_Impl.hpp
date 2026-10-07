#pragma once

// Private to the Renderer implementation files (Renderer.cpp,
// Renderer_Setup.cpp, Renderer_Assets.cpp, Renderer_Passes.cpp): the
// definition of Renderer::Impl. Nothing outside them includes it, so the
// Vulkan stack never reaches the public header.

#include <Renderer.hpp>

#include <Vulkan_Instance.hpp>
#include <Vulkan_Surface.hpp>
#include <Vulkan_Device.hpp>
#include <Vulkan_Allocator.hpp>
#include <Vulkan_Swapchain.hpp>
#include <Vulkan_Render_Pass.hpp>
#include <Swapchain_Targets.hpp>
#include <Vulkan_Handles.hpp>
#include <Vulkan_Pipeline.hpp>
#include <Vulkan_Compute_Pipeline.hpp>
#include <Vulkan_Debug_Utils.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Pipeline_Cache.hpp>
#include <Pipeline_Registry.hpp>
#include <Pipeline_Layout.hpp>
#include <Descriptor_Layout_Cache.hpp>
#include <Geometry_Pool.hpp>
#include <Texture_GPU.hpp>
#include <Storage_Image.hpp>
#include <Sampler_Cache.hpp>
#include <Bindless_Registry.hpp>
#include <Gpu_Timer.hpp>

#include <Renderer_Limits.hpp>
#include <Gpu_Layouts.hpp>
#include <Frame_Data.hpp>
#include <Frame_Timeline.hpp>
#include <Mesh_Registry.hpp>
#include <Upload_Context.hpp>
#include <Material_Table.hpp>
#include <Present_Sync.hpp>
#include <Light_Clusters.hpp>
#include <Draw_List_Builder.hpp>
#include <Frame_Statistics.hpp>

#include <Matrix.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Renderer_System
{

    // =========================================================
    // Frame_Effects
    // =========================================================

    // Frame_Effects: what the recording of a frame leaves pending until the
    // frame is submitted.
    //
    // A state that means "this is already on the GPU" cannot change when a
    // command is recorded: the frame can still fail before its
    // vkQueueSubmit (an exception while recording, a failed submit), and
    // nothing recorded in it will ever run. Record_command_buffer describes
    // such effects here, Render applies them after a successful submit
    // (Apply_effects), and the recovery of a failed frame discards them. A
    // state marked done while recording claims work the GPU never received,
    // and the following frames would skip it.
    //
    // Any new state of that kind, for example a Storage_Image recreated
    // after a resize, is confirmed through this structure and never at
    // recording time.
    struct Frame_Effects
    {
        // The frame recorded the procedural pass: once its submit
        // succeeds, the procedural image is up to date.
        bool             procedural_recorded = false;

        // The frame recorded the update of the cluster boxes for
        // cluster_projection and cluster_near_plane
        // (Light_Clusters::Commit confirms them).
        bool             cluster_boxes_recorded = false;
        MathLib::Matrix4 cluster_projection{ 0.0f };
        float            cluster_near_plane = 0.0f;
    };

    // =========================================================
    // Renderer::Impl
    // =========================================================

    // The Vulkan stack, the subsystems and the sequence of passes.
    //
    // Members are declared in construction order, and destruction runs in
    // reverse: the allocator is declared right after the device, so
    // everything that holds an allocation (depth resources, frames, tables,
    // textures) is gone before it, and the allocator is gone before the
    // device. The synchronization of presentation is declared after the
    // swapchain, so its objects are destroyed before it.
    //
    // A frame is a transaction (see Render): it is either submitted whole,
    // or what it already did to the GPU and to the presentation (the
    // acquired image and the semaphore of its acquire) is undone and the
    // next Render works. When even the undoing fails, or the device is
    // lost, the Renderer is "lost": every operation that would touch the
    // GPU throws at once instead of waiting for work that will never end.
    struct Renderer::Impl
    {
        // =====================================================
        // Constants
        // =====================================================

        static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

        // Fixed capacity of the Geometry_Pool, in elements: 2M vertices
        // (64 MB of Vertex_Static_Mesh) and 8M indices (32 MB of uint32).
        // An upload that does not fit throws; the pool does not grow.
        static constexpr uint32_t GEOMETRY_POOL_VERTICES = 2u * 1024u * 1024u;
        static constexpr uint32_t GEOMETRY_POOL_INDICES = 8u * 1024u * 1024u;

        // Timed scopes one frame may record (Gpu_Timer capacity). A frame
        // records at most nine today: procedural texture, uploads and
        // resets, light clusters, frustum culling and render pass, and
        // inside the render pass opaque, transparent, OIT composite and
        // bounding volumes.
        static constexpr uint32_t GPU_TIMER_MAX_SCOPES = 16;

        // Requested sizes of the two bindless arrays (set 3); the registry
        // clamps them to the device limits and logs both. 1024 textures
        // is generous for a single-scene development workload; raise it if
        // a scene's unique texture count approaches the limit. 16 samplers
        // leave room for the presets still to come (shadow comparison,
        // mirrored wrap...) without touching the layout.
        static constexpr uint32_t BINDLESS_DESIRED_TEXTURES = 1024;
        static constexpr uint32_t BINDLESS_DESIRED_SAMPLERS = 16;

        // Slot i of the sampler array holds Sampler_Preset i, so every
        // preset needs a slot of its own.
        static_assert(static_cast<uint32_t>(CoreTypes::Sampler_Preset::Count) <= BINDLESS_DESIRED_SAMPLERS,
            "The bindless sampler array needs one slot per CoreTypes::Sampler_Preset");

        // =====================================================
        // Vulkan core
        // =====================================================

        // The window is needed after construction to read the size of the
        // framebuffer (the swapchain does not know the window) and to know
        // whether it has one at all (a minimized window cannot have a
        // swapchain).
        const Platform::Window& window;

        Vulkan_Instance         instance;
        Vulkan_Surface          surface;
        Vulkan_Device           device;
        Vulkan_Allocator        allocator;

        // Object names and command labels (no-op without VK_EXT_debug_utils).
        Vulkan_Debug_Utils      debug_utils;

        // =====================================================
        // Geometry
        // =====================================================

        // Vertex and index buffers shared by every mesh, and the table of
        // the meshes that live in them. The registry references the pool.
        Geometry_Pool           geometry_pool;
        Mesh_Registry           mesh_registry;

        // Timestamps around the named scopes of every frame.
        Gpu_Timer               gpu_timer;

        // =====================================================
        // Presentation targets
        // =====================================================

        Vulkan_Swapchain        swapchain;
        Vulkan_Render_Pass      render_pass;

        // The depth buffer, the accumulation and revealage targets of the
        // transparent subpass (read by the composite subpass) and the
        // framebuffers. All of them are sized by the swapchain and
        // recreated with it, as one set.
        Swapchain_Targets       targets;

        // =====================================================
        // Pipelines
        // =====================================================

        // Bindless registry must exist BEFORE the pipeline layout, which
        // references bindless_registry.Get_layout() as its set 3.
        Bindless_Registry       bindless_registry;

        // Must be declared BEFORE the pipelines: they are created through
        // the cache. Destruction runs in reverse, so the pipelines are gone
        // before the cache is serialized.
        Pipeline_Cache          pipeline_cache;
        Descriptor_Layout_Cache descriptor_layouts;

        // The layout every graphics pipeline shares, without push
        // constants. Before the registry, because pipelines are built
        // against it and must be destroyed before it.
        Pipeline_Layout         pipeline_layout;

        // All graphics pipelines, keyed by config.
        Pipeline_Registry       pipeline_registry;

        // Layout of the compute pipelines: the same four set layouts as
        // pipeline_layout, with a push constant range of
        // COMPUTE_PUSH_CONSTANT_SIZE bytes for the compute stage only,
        // shared by every compute pipeline (each shader declares its own
        // block inside it). Declared before the compute pipelines, which
        // are built against it and must be destroyed before it.
        Pipeline_Layout         compute_pipeline_layout;

        // Compute pipelines, held as direct members (no registry: their
        // number is small and fixed). Recorded before the render pass. The
        // cluster pass belongs to Light_Clusters.
        Vulkan_Compute_Pipeline procedural_pipeline;   // procedural.comp
        Vulkan_Compute_Pipeline cull_pipeline;         // cull_objects.comp

        // Clustered lighting: boxes buffer and cluster_lights.comp.
        Light_Clusters          light_clusters;

        // The two pipelines of the mesh passes: opaque (subpass 0, no
        // blending, depth write) and transparent (subpass 1, accumulation
        // into the two OIT targets, depth test only). Held as members so
        // the per-frame lookup never rebuilds and hashes two std::strings.
        Pipeline_Config         opaque_config;
        Pipeline_Config         transparent_config;
        uint8_t                 opaque_pipeline_id = 0;
        uint8_t                 transparent_pipeline_id = 0;

        // Debug pipeline of the bounding volumes (bounds.vert/.frag),
        // subpass 2: wireframe when fillModeNonSolid is enabled, blended
        // filled ellipsoids otherwise.
        Pipeline_Config         bounds_config;
        uint8_t                 bounds_pipeline_id = 0;

        // Composite of the weighted blended OIT (oit_composite.vert/.frag),
        // subpass 2: one full-screen triangle, no vertex input, "over"
        // blending onto the swapchain image.
        Pipeline_Config         composite_config;
        uint8_t                 composite_pipeline_id = 0;

        // Rasterization state shared by every batch; the transparent pass
        // only overrides depth writes. front_face is the winding of the
        // objects whose transform keeps it; the mirrored ones use the
        // opposite (Draw_List_Builder).
        Raster_State            raster_state;

        // Shared sampler configurations, deduplicated.
        Sampler_Cache           sampler_cache;

        // =====================================================
        // Subsystems
        // =====================================================

        Frame_Timeline          timeline;

        // Timeline semaphore that tracks the progress of the frames: every
        // submission of a frame signals the next serial of `timeline`, so a
        // serial N has completed when the semaphore reached N. Waiting for
        // a frame slot is a wait for the serial of its last submission,
        // which returns at once for a slot that never submitted. Nothing
        // has to be reset before a submit, so a failed submit can never
        // leave a wait that no submission will satisfy.
        Unique_Semaphore        frame_semaphore;

        Upload_Context          upload_context;
        Material_Table          material_table;
        Draw_List_Builder       draw_list;
        Frame_Statistics        statistics;

        // =====================================================
        // Frame resources
        // =====================================================

        std::vector<Frame_Data> frames;
        uint32_t                current_frame = 0;

        // Per swapchain image; declared after the swapchain it belongs to.
        Present_Sync            present_sync;

        // =====================================================
        // Descriptors
        // =====================================================

        VkDescriptorPool                              descriptor_pool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, FRAMES_IN_FLIGHT> descriptor_sets{};

        // Set 1 of the compute pipelines (Binding_Per_Pass): the storage
        // image of the procedural pass and the cluster boxes. One set, not
        // one per frame in flight: the descriptors never change, and the
        // resources are shared by both frame slots (see procedural_image).
        VkDescriptorSet         per_pass_set = VK_NULL_HANDLE;

        // Set 1 of the graphics pipelines (Binding_Graphics_Pass): the two
        // OIT targets as input attachments of the composite subpass. One
        // set: the targets are shared by both frame slots, like the depth
        // buffer. Rewritten by Write_composite_input_set whenever the
        // targets are recreated (after the idle wait of Recreate_swapchain).
        VkDescriptorSet         composite_input_set = VK_NULL_HANDLE;

        // Set 2: the material table and the mesh table. One set, written
        // once in Init_global_tables().
        VkDescriptorSet         per_material_set = VK_NULL_HANDLE;

        // MAX_MESHES entries of Mesh_Info_GPU, device-local, written by the
        // upload that creates each mesh (entry = mesh gpu id). Read by the
        // culling pass and the bounding volume debug draw. Entries are
        // written once and never while a frame may read them.
        Vulkan_Buffer_Utils::Buffer_Allocation mesh_table_buffer;

        // =====================================================
        // Compute pass resources
        // =====================================================

        // Output of procedural.comp, sampled by the draws through its
        // bindless slot. Fixed size, independent of the swapchain, so it is
        // never recreated on resize.
        //
        // Written only when its content changes (procedural_dirty), not in
        // every frame: its content depends on nothing that changes per
        // frame today, and every write serializes frames. The barrier of
        // Storage_Image::Begin_write waits for the fragment and compute
        // stages of everything submitted before (the previous frame
        // included), and End_write makes the compute work of the same frame
        // wait for the dispatch.
        //
        // One image for both frames in flight: in a frame that regenerates
        // it, frame N+1 writes it while frame N may still read it.
        // Begin_write orders that write-after-read.
        //
        // std::optional because the constructor records the initial clear
        // into a command buffer: it is emplaced by Init_procedural_pass()
        // and reset in Destroy_owned_handles().
        std::optional<Storage_Image> procedural_image;

        // The content of procedural_image is out of date: the next frame
        // records the procedural pass. Set by Init_procedural_pass; to be
        // set again whenever the content changes (new parameters of the
        // pass, or the image recreated). Cleared only after the submit of a
        // frame that recorded the pass succeeded (Frame_Effects, applied by
        // Apply_effects), so a recording that throws or a failed submit
        // leaves the pass for the next frame.
        //
        // An animated texture (milestone 1.3) changes every frame and would
        // keep the flag set, bringing the serialization back; it then needs
        // one image per frame in flight, or the pass moved after the
        // culling with FRAGMENT as its only reader stage.
        bool                    procedural_dirty = false;

        // Bindless slot of procedural_image (set 3, binding 0): the value a
        // material stores as its albedo texture index. Starts at the Error
        // default texture, so a read before Init_procedural_pass() shows
        // magenta instead of an unregistered slot.
        uint32_t                procedural_texture_index = Default_Texture::Error;

        // =====================================================
        // Assets
        // =====================================================

        // Textures are never removed during a session; the vector only
        // keeps the GPU images alive.
        std::vector<Texture_GPU> textures;

        // =====================================================
        // Per-frame state
        // =====================================================

        Render_Debug_Settings   debug_settings;

        // Lights written to the light buffer this frame, and how many of
        // them (the first ones) are directional: the range the cluster
        // pass distributes is [directional, count).
        uint32_t                uploaded_light_count = 0;
        uint32_t                uploaded_directional_light_count = 0;

        // Near distance the cluster grid of this frame is built for: the
        // packet's near plane, or CLUSTER_FALLBACK_NEAR_DISTANCE when that
        // is not a positive, finite distance (reported by
        // Write_frame_uniforms). The uniforms and the cluster boxes use this
        // one value, so they cannot disagree.
        float                   uploaded_cluster_near_plane = CLUSTER_FALLBACK_NEAR_DISTANCE;

        // The previous packet had an unusable near plane: the report is made
        // when that starts, not on every frame it lasts.
        bool                    reported_invalid_near_plane = false;

        // Mesh of the unit sphere drawn by the bounding volume debug view:
        // bounds.vert places it on every object's local bounding sphere and
        // transforms it by the model matrix, which yields the ellipsoid the
        // culling tests.
        uint32_t                bounds_sphere_mesh_id = 0;

        // Scratch for Gpu_Timer::Read_frame.
        Gpu_Frame_Timings       timer_frame;

        // Last bind count printed, so the log only speaks when it changes.
        uint32_t                last_reported_binds = 0xFFFFFFFF;

        // Light count of the last packet reported as exceeding MAX_LIGHTS;
        // 0 while the packets fit. The overflow is reported when it starts
        // and whenever the count changes, not on every frame.
        uint32_t                reported_light_overflow = 0;

        // The swapchain has to be rebuilt: set by Notify_framebuffer_resized
        // and whenever the driver reports it out of date or suboptimal,
        // cleared only when a recreation completed. While it is set, Render
        // acquires nothing, like a minimized window: a recreation that
        // failed leaves the objects that depend on the swapchain unusable
        // and this flag set, and the next iteration of the loop retries.
        // Only Recreate_swapchain_if_needed acts on it.
        bool                    swapchain_recreation_pending = false;

        // The device is lost, the recovery of a failed frame failed, or a
        // failed transfer could not be waited for (see Is_lost in
        // Renderer.hpp). Never cleared.
        bool                    device_lost = false;

        // vkReleaseSwapchainImagesKHR (or its EXT predecessor), loaded when
        // swapchain_maintenance1 is enabled; null otherwise. Gives back an
        // image that was acquired and never presented.
        PFN_vkReleaseSwapchainImagesKHR release_swapchain_images = nullptr;

        // =====================================================
        // Construction
        // =====================================================

        Impl(const Platform::Window& _window, Validation_Mode _validation_mode);
        ~Impl();

        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;
        Impl(Impl&&) = delete;
        Impl& operator=(Impl&&) = delete;

        // Blocks until the device is idle, so nothing the GPU may still be
        // executing (a transfer that failed and could not be waited for, see
        // Settle_failed_transfer) refers to what is about to be released. A
        // device that cannot go idle (lost) is reported and is not a reason
        // to skip the release. Used by the destructor and by the
        // constructor's failure path, before Destroy_owned_handles. Does not
        // throw.
        void Wait_for_device_before_release() noexcept;

        // Destroys every handle owned directly (not through a member's
        // destructor). Used by the destructor and by the constructor's
        // failure path.
        void Destroy_owned_handles();

        void Init_descriptor_pool();
        void Init_descriptor_sets();

        // Allocates composite_input_set and writes it. Called once from the
        // constructor, after the descriptor pool exists.
        void Init_composite_input_set();

        // Writes the views of the current OIT targets into
        // composite_input_set. Precondition: no command buffer pending
        // execution uses the set (before the first frame, or after the idle
        // wait of Recreate_swapchain).
        void Write_composite_input_set();

        // Uploads the CoreTypes::Default_Texture set in a single batch and
        // checks that each texture landed in its reserved bindless slot.
        // Called once from the constructor, before any other upload.
        void Upload_default_textures();

        // Creates procedural_image and records its initial clear in a
        // one-off transfer submission, registers its view in the bindless
        // set (procedural_texture_index) and allocates and writes
        // per_pass_set with the same view as a storage image. Called once
        // from the constructor, after Upload_default_textures (the default
        // textures keep their reserved slots) and after the descriptor pool
        // exists.
        void Init_procedural_pass();

        // Creates mesh_table_buffer, allocates per_material_set and writes
        // both of its bindings, and registers the default material,
        // checking that it lands in slot CoreTypes::Default_Material.
        // Called once from the constructor, after the descriptor pool
        // exists, after Upload_default_textures (the default material
        // samples Default_Texture::White), before any other material is
        // registered and before any mesh upload (uploads write the mesh
        // table).
        void Init_global_tables();

        // Writes the cluster boxes into per_pass_set. Called once from the
        // constructor, after Init_procedural_pass allocated per_pass_set.
        void Init_light_clusters();

        // Uploads the unit sphere of the bounding volume debug view. Called
        // once from the constructor, after Init_global_tables.
        void Init_debug_meshes();

        // Names the resources created at startup (VK_EXT_debug_utils).
        void Name_debug_objects();

        // Names the OIT targets, which are recreated with the swapchain.
        void Name_oit_targets();

        // =====================================================
        // Assets
        // =====================================================

        Upload_Batch_Result Upload_batch(const Upload_Batch& _batch);
        uint32_t Upload_mesh(const CoreTypes::MeshData& _mesh_data);
        uint32_t Upload_texture(const CoreTypes::ImageData& _image_data, CoreTypes::Pixel_Format _format);
        void Release_mesh(uint32_t _gpu_id);
        uint32_t Register_material(const Material_Desc& _desc);

        // Blocks until every submitted frame has completed (the serial of
        // the last submission is reached); the geometry retired by those
        // frames is freed (Wait_for_serial). Used before an upload writes
        // buffers that frames in flight read, and before it reserves the
        // ranges it needs, so that geometry waiting for those frames can be
        // reused by the same batch.
        void Wait_for_frames_in_flight();

        // =====================================================
        // Frame
        // =====================================================

        // Draws one frame as a transaction:
        //   1. wait for the slot: the serial of its last submission;
        //   2. the CPU work that does not depend on the swapchain image
        //      (Prepare_frame): draw lists, object buffer, uniforms. It
        //      comes before the acquire so that a failure there holds no
        //      image, and an acquired image is held for as little time as
        //      possible;
        //   3. acquire the image;
        //   4. the protected region, up to and including vkQueueSubmit. If
        //      anything in it throws, Recover_acquired_image undoes the
        //      acquire (the semaphore signal pending on the slot, and the
        //      image), the effects of the frame are discarded, and the
        //      exception propagates;
        //   5. after the submit the frame is committed: its effects are
        //      applied (Apply_effects), then the present.
        // The slot advances in every case once the acquire succeeded.
        // Render does not recreate the swapchain in reaction to what the
        // acquire or the present report: an out-of-date or suboptimal
        // result sets swapchain_recreation_pending and the loop recreates
        // it before the next frame (Recreate_swapchain_if_needed). The only
        // recreation inside Render is the one at its start, for a pending
        // request the loop did not apply (a safety net for callers that
        // bypass the loop); it never blocks.
        void Render(const RenderPacket& _packet);

        // The body of Render; Render adds the marking of a lost device.
        void Render_frame(const RenderPacket& _packet);

        // The CPU work of the frame that does not depend on the swapchain
        // image: the culling frustum, the draw lists and the object buffer,
        // the CPU-written indirect commands when the path needs them, and
        // the uniforms. Runs after the wait for the slot.
        void Prepare_frame(Frame_Data& _frame, const RenderPacket& _packet);

        // Copies the packet's view and lights into the frame's mapped
        // buffers, together with the draw buckets of the frame.
        void Write_frame_uniforms(Frame_Data& _frame, const RenderPacket& _packet);

        // Records all render commands for one frame into the command
        // buffer of the given frame slot. Prepare_frame has run. What the
        // recording leaves pending until the submit is described in
        // _out_effects; nothing that means "already on the GPU" changes
        // here.
        void Record_command_buffer(Frame_Data& _frame, const RenderPacket& _packet,
                                   uint32_t _image_index, Frame_Effects& _out_effects);

        // What one submission to the graphics queue waits for, executes and
        // signals besides the timeline semaphore, which every submission of
        // a frame signals with the next serial.
        struct Timeline_Submit
        {
            // Binary semaphore signaled by the acquire, waited on at
            // wait_stage.
            VkSemaphore          wait_semaphore = VK_NULL_HANDLE;
            VkPipelineStageFlags wait_stage = 0;

            // The commands to execute; null for a submission without any.
            VkCommandBuffer      command_buffer = VK_NULL_HANDLE;

            // Binary semaphore signaled for the present; null for none.
            VkSemaphore          signal_semaphore = VK_NULL_HANDLE;

            // Names the submission in the message of the exception thrown
            // when vkQueueSubmit fails.
            const char*          what = "Renderer: failed to submit";
        };

        // Submits _submit and makes it the next serial of the timeline for
        // the current frame slot. The serial is consumed
        // (Frame_Timeline::Record_submission) only when the submission
        // succeeded; a failed one leaves the timeline as it was, and the
        // next submission signals the same value. The one place that
        // builds a frame's submit, for the frame itself (Submit_frame) and
        // for the recovery of a frame that failed (Recover_acquired_image).
        void Submit_to_timeline(const Timeline_Submit& _submit);

        // Submits the recorded command buffer of _frame: waits for the
        // acquire semaphore, signals _render_finished for the present and
        // the next serial of frame_semaphore (Submit_to_timeline).
        void Submit_frame(Frame_Data& _frame, VkSemaphore _render_finished);

        // Applies the effects of a frame that was submitted.
        void Apply_effects(const Frame_Effects& _effects);

        // Undoes the acquire of a frame that will not be presented, after
        // an exception in the protected region of Render:
        //   - a submission without commands consumes the signal of the
        //     acquire semaphore (which cannot be waited on again until
        //     then) and signals the next serial, registered as the
        //     slot's submission, so the wait for the slot also waits for
        //     that semaphore to become reusable;
        //   - the image goes back to the swapchain
        //     (vkReleaseSwapchainImages), or, without swapchain
        //     maintenance1, the swapchain is marked for recreation:
        //     destroying it frees the images that were acquired and never
        //     presented.
        // If the submission cannot be made the Renderer becomes lost. Does
        // not throw.
        void Recover_acquired_image(Frame_Data& _frame, uint32_t _image_index) noexcept;

        // Draw recording inside the render pass. The geometry pool and the
        // descriptor sets are already bound. Draw_State tracks the state the
        // command buffer holds between the draws, so it is only set when it
        // changes.
        //   Record_opaque_draws      - subpass 0, with the path the draw
        //                              list resolved.
        //   Record_transparent_draws - subpass 1, depth writes already
        //                              disabled.
        //   Record_direct_draws      - the draw loop of both, when the CPU
        //                              records one draw per object.
        //   Record_composite_draw    - subpass 2: the full-screen triangle
        //                              that resolves the OIT targets.
        //   Record_bounds_draw       - subpass 2, after the composite.
        struct Draw_State
        {
            // The graphics pipeline currently bound, null until the first
            // bind. A handle and not an id: the ids of Pipeline_Registry use
            // every value of a uint8_t, so none can mean "nothing bound",
            // while a null handle is never a valid pipeline.
            VkPipeline  bound_pipeline = VK_NULL_HANDLE;

            // Front face currently set.
            VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

            // Pipeline binds, counted for the log.
            uint32_t    bind_count = 0;
        };

        void Record_opaque_draws(VkCommandBuffer _command_buffer, Frame_Data& _frame, Draw_State& _state);
        void Record_transparent_draws(VkCommandBuffer _command_buffer, Draw_State& _state);
        void Record_composite_draw(VkCommandBuffer _command_buffer, Draw_State& _state);
        void Record_bounds_draw(VkCommandBuffer _command_buffer, Draw_State& _state);

        // One vkCmdDrawIndexed per entry of _draws, straight from the pool,
        // in order. The pipeline and the winding are set only when they
        // change from one draw to the next. The recording of the transparent
        // subpass, and of the opaque one on the Direct path.
        void Record_direct_draws(VkCommandBuffer _command_buffer,
                                 const std::vector<Draw_List_Builder::Draw_Record>& _draws, Draw_State& _state);

        // Binds graphics pipeline _pipeline_id unless it is already bound.
        void Bind_graphics_pipeline(VkCommandBuffer _command_buffer, uint8_t _pipeline_id, Draw_State& _state);

        // Sets the front face for objects that keep the winding of their
        // mesh (_mirrored false) or invert it, unless it is already set.
        void Set_front_face(VkCommandBuffer _command_buffer, bool _mirrored, Draw_State& _state);

        // Reads what the GPU measured for the last frame recorded in
        // _frame_slot (timestamps and counters) and hands it to the
        // statistics. Called right after the slot was waited on.
        void Read_frame_statistics(uint32_t _frame_slot);

        // =====================================================
        // Progress of the GPU
        // =====================================================

        // Blocks until frame_semaphore reaches _serial, then tells the
        // timeline how far the GPU got and frees the geometry that the
        // frames now known to be complete were holding. The blocking is
        // skipped for serial 0 and for serials already known to be complete;
        // the counter is read, and the completed geometry freed, in every
        // case (the GPU may have gone beyond _serial). Throws on a wait
        // error.
        void Wait_for_serial(uint64_t _serial, const char* _what);

        // Blocks until the device is idle, then tells the timeline that
        // every submitted frame completed and frees the geometry they were
        // holding. For work that must not overlap anything on the GPU (the
        // swapchain recreation). _what names the wait in the message of the
        // exception thrown on failure.
        void Wait_idle(const char* _what);

        // Throws std::runtime_error at once, without touching the GPU, when
        // the Renderer is lost. _operation names the caller in the message.
        void Require_not_lost(const char* _operation) const;

        // Marks the Renderer as lost when the exception being handled is a
        // Vulkan_Error with VK_ERROR_DEVICE_LOST. Called from the catch
        // block of an operation that touches the GPU, before it rethrows.
        // Does not throw.
        void Note_current_failure() noexcept;

        // Called from the catch block of a failed transfer
        // (Upload_Context::Run), before it releases anything the transfer
        // wrote. True: the transfer is not running any more, so the handler
        // can release what the transfer targeted. False: Run could not wait
        // for the device (Upload_Context::Is_usable), so the GPU may still
        // be writing there; the Renderer is lost, like when the recovery of
        // a failed frame fails, the handler leaves those resources alone and
        // the destructor releases them after its own wait. Does not throw.
        bool Settle_failed_transfer() noexcept;

        // =====================================================
        // Surface size
        // =====================================================

        // The only place that recreates the swapchain, called by the engine
        // loop before it builds the frame. Returns true when the swapchain
        // is usable afterwards; false while the window is minimized or the
        // surface has no area (nothing is torn down then). Throws if the
        // recreation fails; the flag stays set and the next call retries.
        bool Recreate_swapchain_if_needed();

        // Recreates the swapchain, the targets that follow it (depth
        // buffer, OIT targets, framebuffers: Swapchain_Targets), the
        // descriptors that read the OIT targets and the per-image
        // synchronization after a resize or OUT_OF_DATE error. Pipelines
        // are unaffected (viewport/scissor are dynamic).
        //
        // The swapchain keeps the format of the first one: the render pass
        // and the pipelines are built for it, so a surface that no longer
        // offers it makes the recreation throw (Vulkan_Swapchain::
        // Can_recreate) instead of producing framebuffers the render pass
        // cannot use. It throws before the device wait and before anything
        // is retired: the old swapchain and its objects stay as they were,
        // and every retry is as cheap as the query of the surface.
        //
        // Transactional: everything that can fail is created first, in
        // temporaries, and only then are the members replaced by moving. If
        // a creation throws, the temporaries release themselves,
        // swapchain_recreation_pending stays set, and the objects that
        // depend on the old swapchain are not used (Render acquires
        // nothing while the flag is set: it retries the recreation first
        // and returns if it cannot). Returns false, touching nothing, when
        // the surface has no area. Waits for the device to go idle, so it
        // must not run while a frame is being recorded.
        bool Recreate_swapchain();

        void Set_debug_settings(const Render_Debug_Settings& _settings);

        std::vector<Pipeline_Config> Build_pipeline_manifest() const;
    };

} // namespace Renderer_System
