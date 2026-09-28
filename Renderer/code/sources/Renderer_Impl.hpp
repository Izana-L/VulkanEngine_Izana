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
#include <Vulkan_Depth_Resources.hpp>
#include <Vulkan_OIT_Resources.hpp>
#include <Vulkan_Framebuffer.hpp>
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

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Renderer_System
{

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

        // The window is needed after construction to know whether the
        // framebuffer has a size at all (a minimized window cannot have a
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
        Vulkan_Depth_Resources  depth_resources;

        // Accumulation and revealage targets of the transparent subpass,
        // read by the composite subpass. Screen sized: recreated with the
        // swapchain. Declared before the framebuffers, which reference them.
        Vulkan_OIT_Resources    oit_resources;

        Vulkan_Framebuffer      framebuffers;

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
        // only overrides depth writes.
        Raster_State            raster_state;

        // Shared sampler configurations, deduplicated.
        Sampler_Cache           sampler_cache;

        // =====================================================
        // Subsystems
        // =====================================================

        Frame_Timeline          timeline;
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
        // frame that recorded the pass succeeded, so a recording that
        // throws or a failed submit leaves the pass for the next frame.
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
        uint32_t                procedural_texture_index = CoreTypes::Default_Texture::Error;

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

        // Set by Notify_framebuffer_resized, consumed by
        // Recreate_swapchain_if_needed.
        bool                    framebuffer_resized = false;

        // =====================================================
        // Construction
        // =====================================================

        Impl(const Platform::Window& _window, Validation_Mode _validation_mode);
        ~Impl();

        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;
        Impl(Impl&&) = delete;
        Impl& operator=(Impl&&) = delete;

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

        // Blocks until every submitted frame has completed (the fences of
        // all frame slots), then frees the retired geometry. Used before
        // an upload writes buffers that frames in flight read.
        void Wait_for_frames_in_flight();

        // =====================================================
        // Frame
        // =====================================================

        void Render(const CoreTypes::RenderPacket& _packet);

        // Copies the packet's view and lights into the frame's mapped buffers.
        void Write_frame_uniforms(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet);

        // Records all render commands for one frame into the command
        // buffer of the given frame slot.
        void Record_command_buffer(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet, uint32_t _image_index);

        // Draw recording inside the render pass. The geometry pool and the
        // descriptor sets are already bound. _bound_pipeline_id is the id
        // of the graphics pipeline currently bound (0xFF: none);
        // _bind_count counts pipeline binds for the log.
        //   Record_opaque_draws      - subpass 0.
        //   Record_transparent_draws - subpass 1, depth writes already
        //                              disabled.
        //   Record_composite_draw    - subpass 2: the full-screen triangle
        //                              that resolves the OIT targets.
        //   Record_bounds_draw       - subpass 2, after the composite.
        void Record_opaque_draws(VkCommandBuffer _command_buffer, Frame_Data& _frame, Opaque_Draw_Path _path,
                                 uint8_t& _bound_pipeline_id, uint32_t& _bind_count);
        void Record_transparent_draws(VkCommandBuffer _command_buffer, uint8_t& _bound_pipeline_id, uint32_t& _bind_count);
        void Record_composite_draw(VkCommandBuffer _command_buffer, uint8_t& _bound_pipeline_id, uint32_t& _bind_count);
        void Record_bounds_draw(VkCommandBuffer _command_buffer, uint8_t& _bound_pipeline_id, uint32_t& _bind_count);

        // Binds graphics pipeline _pipeline_id unless it is already bound.
        void Bind_graphics_pipeline(VkCommandBuffer _command_buffer, uint8_t _pipeline_id,
                                    uint8_t& _bound_pipeline_id, uint32_t& _bind_count);

        // Reads what the GPU measured for the last frame recorded in
        // _frame_slot (timestamps and counters) and hands it to the
        // statistics. Called right after the fence of the slot was waited on.
        void Read_frame_statistics(uint32_t _frame_slot);

        // =====================================================
        // Surface size
        // =====================================================

        bool Recreate_swapchain_if_needed();

        // Recreates the swapchain, depth resources, OIT targets (and the
        // descriptors that read them), framebuffers and per-image
        // synchronization after a resize or OUT_OF_DATE error. Pipelines
        // are unaffected (viewport/scissor are dynamic).
        void Recreate_swapchain();

        void Set_debug_settings(const Render_Debug_Settings& _settings);

        std::vector<Pipeline_Config> Build_pipeline_manifest() const;
    };

} // namespace Renderer_System
