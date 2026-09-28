#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

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
#include <Vulkan_Command_Pool.hpp>
#include <Frame_Data.hpp>
#include <Pipeline_Cache.hpp>
#include <Pipeline_Registry.hpp>
#include <Pipeline_Layout.hpp>
#include <Geometry_Pool.hpp>
#include <Mesh_GPU.hpp>
#include <Texture_GPU.hpp>
#include <Storage_Image.hpp>
#include <Sampler_Cache.hpp>
#include <Bindless_Registry.hpp>
#include <Gpu_Timer.hpp>
#include <Vulkan_Debug_Utils.hpp>
#include <RenderPacket.hpp>
#include <ImageData.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace Platform { class Window; }

namespace Renderer_System
{
    // One texture in an upload batch. The format travels per-texture
    // because only the caller knows a texture's role: *_SRGB for color
    // data (albedo, emissive), *_UNORM for data maps (normal,
    // metallic-roughness, AO). Upload_texture(const ImageData&) derives it
    // from ImageData::format through Vulkan_Image_Utils::To_vk_format.
    struct Texture_Upload
    {
        const CoreTypes::ImageData* data = nullptr;
        VkFormat                    format = VK_FORMAT_R8G8B8A8_SRGB;
    };

    // A set of assets to upload together in one command buffer and one
    // submit. Holds NON-OWNING pointers: the caller (ResourceManager) owns
    // the data and only has to keep it alive across the Upload_batch call.
    struct Upload_Batch
    {
        std::vector<const CoreTypes::MeshData*> meshes;
        std::vector<Texture_Upload>             textures;
    };

    // Result of Upload_batch. Each vector is parallel to its input list.
    struct Upload_Batch_Result
    {
        // Index into the Renderer's internal mesh registry: the value
        // ResourceManager stores through Register_gpu_id().
        std::vector<uint32_t> mesh_gpu_ids;

        // Index into the global bindless texture array (set 3, binding 0):
        // what shaders use, NOT the internal texture registry index.
        std::vector<uint32_t> texture_bindless_indices;
    };

    // Material as the Renderer registers it: GPU-ready values only, with
    // every texture already resolved to its bindless index. The caller
    // (Engine) translates an ECS::Material_Component into this, choosing
    // the default textures for unassigned or missing images, so the
    // Renderer never sees asset handles.
    //
    // Two descriptions that compare equal share one slot of the material
    // table: Register_material deduplicates by value.
    struct Material_Desc
    {
        MathLib::Vector4          base_color = { 1.0f, 1.0f, 1.0f, 1.0f };
        uint32_t                  albedo_texture_index = CoreTypes::Default_Texture::White;
        CoreTypes::Sampler_Preset sampler = CoreTypes::Sampler_Preset::Linear_Repeat;

        bool operator==(const Material_Desc& _other) const
        {
            return base_color == _other.base_color
                && albedo_texture_index == _other.albedo_texture_index
                && sampler == _other.sampler;
        }
    };

    // Path of the opaque pass. Every path draws the same objects from the
    // same Geometry_Pool with the same shaders; they differ in who builds
    // the draw commands, which is what makes them comparable at runtime.
    //   Direct       - one vkCmdDrawIndexed per object, recorded by the CPU
    //                  (roadmap milestone 3.1).
    //   Cpu_Indirect - the CPU writes the commands into a host-visible
    //                  buffer; one vkCmdDrawIndexedIndirect per pipeline
    //                  (milestone 3.4).
    //   Gpu_Indirect - cull_objects.comp writes one command per active
    //                  opaque object, without frustum test; one
    //                  vkCmdDrawIndexedIndirectCount (milestone 4.1).
    //   Gpu_Culled   - the same with the frustum test: objects outside the
    //                  culling frustum get no command (milestone 4.2), and
    //                  the transparent items are culled on the CPU against
    //                  the same planes (milestone 4.3).
    // The GPU paths need every opaque item on one pipeline (one command
    // bucket); a frame that mixes opaque pipelines, or has more opaque
    // objects than maxDrawIndirectCount, uses Cpu_Indirect instead.
    enum class Opaque_Draw_Path : uint32_t
    {
        Direct = 0,
        Cpu_Indirect = 1,
        Gpu_Indirect = 2,
        Gpu_Culled = 3,
        Count
    };

    // Runtime switches between the old and the new path of each roadmap
    // step, and the debug views that validate them. Read and written
    // between frames (Renderer::Get_debug_settings / Set_debug_settings);
    // a change applies from the next Render call.
    struct Render_Debug_Settings
    {
        // Light selection of mesh.frag (clustered, or every light).
        Light_Culling_Mode light_culling = Light_Culling_Mode::Clustered;

        // Overlay of the cluster grid.
        Cluster_Debug_View cluster_view = Cluster_Debug_View::None;

        // Light count the heatmap shows as full red.
        uint32_t           heatmap_max_lights = 32;

        Opaque_Draw_Path   opaque_path = Opaque_Draw_Path::Gpu_Culled;

        // Culling camera frozen: the frustum of the frame the freeze was
        // enabled on keeps being used for culling while the view moves,
        // so what is discarded becomes visible.
        bool               freeze_culling = false;

        // Wireframe of the bounding volume the culling tests for every
        // object: its mesh bounding sphere placed by the model matrix, an
        // ellipsoid under non-uniform scale or shear (bounds.vert).
        bool               show_bounds = false;

        // Print GPU timings and counters to the console once per second.
        bool               print_stats = false;

        // Isolated GPU timing (Gpu_Timer): a full barrier before every
        // top-level timed scope, so each one measures its pass alone,
        // without overlap. Changes the performance of the frame: the
        // printed totals are not frame times. Meant to compare one pass
        // before and after a change.
        bool               isolate_gpu_timings = false;
    };

    // Readable names, for logs.
    const char* To_string(Light_Culling_Mode _mode);
    const char* To_string(Cluster_Debug_View _view);
    const char* To_string(Opaque_Draw_Path _path);

    // Renderer: the only Vulkan-facing class that EngineCore knows about.
    //
    // Owns the entire Vulkan stack (instance -> device -> swapchain ->
    // pipelines) and all per-frame resources. Exposes operations to
    // EngineCore:
    //
    //   Upload_mesh()    - uploads geometry to the GPU once at load time.
    //   Upload_texture() - uploads a texture (with mipmaps) once at load time.
    //   Render()         - consumes a RenderPacket and produces one frame.
    //
    // Swapchain recreation happens for two reasons, both handled here:
    //   - the window reported a resize (Notify_framebuffer_resized, called
    //     by the engine loop from Window::Consume_resized_flag); the
    //     swapchain is rebuilt on the next Recreate_swapchain_if_needed()
    //     or Render() call, before the frame's aspect ratio is computed;
    //   - the driver reported OUT_OF_DATE / SUBOPTIMAL.
    //
    // Frame structure (Record_command_buffer), everything GPU-driven
    // recorded before the render pass:
    //   compute  - procedural texture, only in the frames where its content
    //              changes (procedural_dirty): once at startup today;
    //   transfer - cluster boxes (when the projection changed) and counter
    //              resets;
    //   compute  - light assignment to clusters, frustum culling of the
    //              opaque objects;
    //   barrier  - compute writes to the fragment stage, the indirect
    //              command read and the statistics copy;
    //   render   - one geometry bind, then three subpasses:
    //                0 opaque      - opaque draws (direct, CPU indirect or
    //                                GPU indirect count), depth written;
    //                1 transparent - transparent draws accumulated into the
    //                                OIT targets (weighted blended
    //                                order-independent transparency), in
    //                                any order, depth tested only;
    //                2 composite   - the accumulated transparency blended
    //                                over the opaque color, then the debug
    //                                bounding volumes;
    //   transfer - statistics copies into the readback buffer.
    //
    // Every block is a named Gpu_Scope: the same name labels it in captures
    // and times it in the statistics (Render_Debug_Settings::print_stats).
    //
    // Presentation synchronization: the "render finished" semaphore and
    // the present fence belong to each SWAPCHAIN IMAGE, not to the frame
    // slot. A present may still be waiting on the semaphore of an image
    // while the frame slot that recorded it is reused, and there are more
    // images than slots, so per-slot semaphores would be re-signaled while
    // pending. With VK_KHR_swapchain_maintenance1 the present fence tells
    // exactly when an image's present completed; without it, per-image
    // semaphores are the guarantee, and semaphores of a destroyed
    // swapchain are retired for a few frames before being destroyed.
    //
    // Not copyable or movable: owns the entire Vulkan lifetime.
    class Renderer
    {
    private:

        // The window is needed after construction to know whether the
        // framebuffer has a size at all (a minimized window cannot have a
        // swapchain). Must outlive the Renderer.
        const Platform::Window& window;

        // Vulkan core - construction order matters for destruction
        Vulkan_Instance        instance;
        Vulkan_Surface         surface;
        Vulkan_Device          device;

        // Declared right after device on purpose. Construction runs in
        // declaration order, so instance and device are ready when the
        // allocator is built; destruction runs in reverse, so everything
        // holding an allocation (depth_resources, meshes, textures,
        // frames) is gone before the allocator, and the allocator is gone
        // before the device.
        Vulkan_Allocator       allocator;

        // Object names and command labels (no-op without VK_EXT_debug_utils).
        Vulkan_Debug_Utils     debug_utils;

        // Vertex and index buffers shared by every mesh. Declared after the
        // allocator: it holds two allocations and must be destroyed first.
        Geometry_Pool          geometry_pool;

        // Timestamps around the named scopes of every frame.
        Gpu_Timer              gpu_timer;

        Vulkan_Swapchain       swapchain;
        Vulkan_Render_Pass     render_pass;
        Vulkan_Depth_Resources depth_resources;

        // Accumulation and revealage targets of the transparent subpass,
        // read by the composite subpass. Screen sized: recreated with the
        // swapchain. Declared before the framebuffers, which reference them.
        Vulkan_OIT_Resources   oit_resources;

        Vulkan_Framebuffer     framebuffers;

        // Bindless registry must exist BEFORE the pipeline layout, which
        // references bindless_registry.Get_layout() as its set 3.
        Bindless_Registry      bindless_registry;
        // Must be declared BEFORE the pipelines: they are created through
        // the cache. Destruction runs in reverse, so the pipelines are gone
        // before the cache is serialized.
        Pipeline_Cache         pipeline_cache;
        Descriptor_Layout_Cache descriptor_layouts;
        // The layout every graphics pipeline shares. Before the registry,
        // because pipelines are built against it and must be destroyed
        // before it.
        Pipeline_Layout        pipeline_layout;

        // All graphics pipelines, keyed by config.
        Pipeline_Registry      pipeline_registry;

        // Layout of the compute pipelines: the same four set layouts as
        // pipeline_layout, with a push constant range of
        // COMPUTE_PUSH_CONSTANT_SIZE bytes for the compute stage only,
        // shared by every compute pipeline (each shader declares its own
        // block inside it). Declared before the compute pipelines, which
        // are built against it and must be destroyed before it.
        Pipeline_Layout        compute_pipeline_layout;

        // Compute pipelines, held as direct members (no registry: their
        // number is small and fixed). Recorded before the render pass in
        // Record_command_buffer.
        Vulkan_Compute_Pipeline procedural_pipeline;
        Vulkan_Compute_Pipeline cluster_pipeline;     // cluster_lights.comp
        Vulkan_Compute_Pipeline cull_pipeline;        // cull_objects.comp

        // The two pipelines of the mesh passes: opaque (subpass 0, no
        // blending, depth write) and transparent (subpass 1, accumulation
        // into the two OIT targets, depth test only). Held as members so
        // the per-frame lookup never rebuilds and hashes two std::strings.
        Pipeline_Config        opaque_config;
        Pipeline_Config        transparent_config;
        uint8_t                opaque_pipeline_id = 0;
        uint8_t                transparent_pipeline_id = 0;

        // Debug pipeline of the bounding volumes (bounds.vert/.frag),
        // subpass 2: wireframe when fillModeNonSolid is enabled, blended
        // filled ellipsoids otherwise.
        Pipeline_Config        bounds_config;
        uint8_t                bounds_pipeline_id = 0;

        // Composite of the weighted blended OIT (oit_composite.vert/.frag),
        // subpass 2: one full-screen triangle, no vertex input, "over"
        // blending onto the swapchain image.
        Pipeline_Config        composite_config;
        uint8_t                composite_pipeline_id = 0;

        // Last bind count printed, so the log only speaks when it changes.
        // A member, not a function-level static: a static would be shared
        // by every Renderer in the process and is not reentrant.
        uint32_t               last_reported_binds = 0xFFFFFFFF;

        // Draw items that reference a mesh, transform or pipeline that
        // does not exist are skipped; the first one is reported once.
        bool                   warned_invalid_item = false;

        // Draws beyond MAX_OBJECTS in one frame are skipped; the first
        // frame that overflows is reported once.
        bool                   warned_object_overflow = false;

        // Draw items whose texture index is not a registered bindless
        // slot, or whose sampler index is not a Sampler_Preset, are drawn
        // with a valid fallback; the first one is reported once.
        bool                   warned_invalid_material_index = false;

        // Light count of the last packet reported as exceeding MAX_LIGHTS;
        // 0 while the packets fit. The overflow is reported when it starts
        // and whenever the count changes, not on every frame.
        uint32_t               reported_light_overflow = 0;

        // A GPU draw path was requested for a frame whose opaque items use
        // several pipelines (or exceed maxDrawIndirectCount); reported once.
        bool                   warned_gpu_path_fallback = false;

        // The cluster light index list overflowed; reported once.
        bool                   warned_cluster_overflow = false;

        // Rasterization state shared by every batch; the transparent pass
        // only overrides depth writes.
        Raster_State           raster_state;

        // Shared sampler configurations, deduplicated.
        Sampler_Cache          sampler_cache;

        // =========================================================
        // Frame resources
        // =========================================================

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

        std::vector<Frame_Data> frames;
        uint32_t                current_frame = 0;

        // Per swapchain image (see the class comment).
        struct Image_Sync
        {
            // Signaled by the graphics queue when rendering into this image
            // completes; the present of this image waits on it.
            VkSemaphore render_finished = VK_NULL_HANDLE;

            // VK_KHR_swapchain_maintenance1 only: signaled when the present
            // of this image completes. VK_NULL_HANDLE otherwise.
            VkFence     present_fence = VK_NULL_HANDLE;

            // True while a present that signals present_fence is pending.
            bool        present_pending = false;
        };

        std::vector<Image_Sync> image_sync;

        // Synchronization objects of a swapchain that no longer exists but
        // whose last presents may still be pending. Destroyed after
        // frames_left further frames (or when their fence reports the
        // present completed).
        struct Retired_Sync
        {
            std::vector<VkSemaphore> semaphores;
            std::vector<VkFence>     fences;
            uint32_t                 frames_left = 0;
        };

        std::vector<Retired_Sync> retired_sync;

        // =========================================================
        // Descriptors
        // =========================================================

        VkDescriptorPool                                  descriptor_pool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, FRAMES_IN_FLIGHT>     descriptor_sets{};

        // Set 1 of the compute pipelines (Binding_Per_Pass): the storage
        // image of the procedural pass and the cluster boxes. One set, not
        // one per frame in flight: the descriptors never change, and the
        // resources are shared by both frame slots (see procedural_image).
        VkDescriptorSet                                   per_pass_set = VK_NULL_HANDLE;

        // Set 1 of the graphics pipelines (Binding_Graphics_Pass): the two
        // OIT targets as input attachments of the composite subpass. One
        // set: the targets are shared by both frame slots, like the depth
        // buffer. Rewritten by Write_composite_input_set whenever the
        // targets are recreated (after the idle wait of Recreate_swapchain).
        VkDescriptorSet                                   composite_input_set = VK_NULL_HANDLE;

        // =========================================================
        // Compute pass resources
        // =========================================================

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
        // into a command buffer: it is emplaced by Init_procedural_pass(),
        // after the transfer pool and fence exist, and reset in
        // Destroy_owned_handles(). Declared after the allocator, so it is
        // always destroyed before it.
        std::optional<Storage_Image>                      procedural_image;

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
        bool                                              procedural_dirty = false;

        // Bindless slot of procedural_image (set 3, binding 0): the value a
        // material stores as its albedo texture index. Starts at the Error
        // default texture, so a read before Init_procedural_pass() shows
        // magenta instead of an unregistered slot.
        uint32_t                                          procedural_texture_index = CoreTypes::Default_Texture::Error;

        // =========================================================
        // Material table (set 2)
        // =========================================================

        // MAX_MATERIALS entries of Material_GPU, storage buffer,
        // persistently mapped. One copy shared by every frame in flight:
        // the table is append-only, so Register_material only writes slots
        // that no submitted command references yet, and host-coherent
        // writes before a submit need no barrier.
        Vulkan_Buffer_Utils::Buffer_Allocation            material_buffer;

        // Set 2: the material table descriptor. One set, written once in
        // Init_material_table().
        VkDescriptorSet                                   per_material_set = VK_NULL_HANDLE;

        // CPU copy of every registered description; index = slot in the
        // table. Used by Register_material to deduplicate by value.
        std::vector<Material_Desc>                        registered_materials;

        // =========================================================
        // Mesh table (set 2)
        // =========================================================

        // MAX_MESHES entries of Mesh_Info_GPU, device-local, written by the
        // upload that creates each mesh (entry = mesh gpu id). Read by the
        // culling pass and the bounding volume debug draw. Entries are
        // written once and never while a frame may read them.
        Vulkan_Buffer_Utils::Buffer_Allocation            mesh_table_buffer;

        // =========================================================
        // Clustered lighting (set 1)
        // =========================================================

        // CLUSTER_COUNT view space boxes, device-local, one copy for every
        // frame in flight. Rewritten inside the frame's command buffer
        // (vkCmdUpdateBuffer) when the projection or the near plane
        // changes, after a barrier against the reads of earlier frames.
        Vulkan_Buffer_Utils::Buffer_Allocation            cluster_aabb_buffer;

        // CPU build of the boxes (Cluster_Grid::Build_aabbs), reused.
        std::vector<Cluster_AABB_GPU>                     cluster_aabb_scratch;

        // Projection and near plane the boxes were built for.
        MathLib::Matrix4                                  cluster_aabb_projection{ 0.0f };
        float                                             cluster_aabb_near = 0.0f;
        bool                                              cluster_aabbs_valid = false;

        // =========================================================
        // Culling and debug state
        // =========================================================

        Render_Debug_Settings                             debug_settings;

        // Frustum used for culling this frame: the packet's, or the frozen
        // one (Render_Debug_Settings::freeze_culling).
        CoreTypes::Frustum                                culling_frustum;
        CoreTypes::Frustum                                frozen_frustum;

        // Set when the freeze is enabled: the next frame copies its frustum
        // into frozen_frustum.
        bool                                              capture_frozen_frustum = false;

        // One drawn object of the frame: its entry in the object buffer, its
        // mesh and its pipeline. Rebuilt every frame by Prepare_objects;
        // members so the capacity is reused instead of reallocated.
        struct Draw_Record
        {
            uint32_t object_index = 0;
            uint32_t mesh_id = 0;
            uint8_t  pipeline_id = 0;
        };

        std::vector<Draw_Record>                          opaque_draws;
        std::vector<Draw_Record>                          transparent_draws;

        // Transparent items of the frame before the CPU frustum test, for
        // the statistics.
        uint32_t                                          transparent_candidates = 0;

        // Lights written to the light buffer this frame, and how many of
        // them (the first ones) are directional: the range the cluster
        // pass distributes is [directional, count).
        uint32_t                                          uploaded_light_count = 0;
        uint32_t                                          uploaded_directional_light_count = 0;

        // Mesh of the unit sphere drawn by the bounding volume debug view:
        // bounds.vert places it on every object's local bounding sphere and
        // transforms it by the model matrix, which yields the ellipsoid the
        // culling tests.
        uint32_t                                          bounds_sphere_mesh_id = 0;

        // =========================================================
        // Statistics
        // =========================================================

        // What a frame slot recorded, kept until its fence is waited on so
        // the GPU counters can be read with their context.
        struct Frame_Record
        {
            bool             recorded = false;
            Opaque_Draw_Path opaque_path = Opaque_Draw_Path::Direct;
            uint32_t         opaque_objects = 0;
            uint32_t         transparent_candidates = 0;
            uint32_t         transparent_drawn = 0;

            // The frame recorded the procedural pass: once its submit
            // succeeds, procedural_image is up to date.
            bool             procedural_recorded = false;
        };

        std::array<Frame_Record, FRAMES_IN_FLIGHT>        frame_records{};

        // Accumulated time of one timed scope between two prints, keyed by
        // name and parent name.
        struct Scope_Statistics
        {
            const char* name = nullptr;
            const char* parent = nullptr;
            uint32_t    depth = 0;
            uint32_t    frames = 0;       // timed frames that recorded the scope
            double      ms_sum = 0.0;
        };

        // Accumulated between two prints (Render_Debug_Settings::print_stats).
        struct Frame_Statistics
        {
            uint32_t                                  frames = 0;
            uint32_t                                  timed_frames = 0;
            double                                    total_ms_sum = 0.0;   // frame end - frame start
            double                                    other_ms_sum = 0.0;   // total - top-level scopes

            // In recording order; a scope first seen in a later frame is
            // inserted after the scope that preceded it in that frame.
            std::vector<Scope_Statistics>             scopes;

            Frame_Stats_GPU                           last_counters{};
            Frame_Record                              last_record{};
            std::chrono::steady_clock::time_point     last_print{};
        };

        Frame_Statistics                                  statistics;

        // Scratch for Gpu_Timer::Read_frame.
        Gpu_Frame_Timings                                 timer_frame;


        // =========================================================
        // Asset registries
        // =========================================================

        // gpu_id = index into this vector.
        // gpu ids are never recycled: a released mesh keeps its entry
        // (Mesh_GPU::released), so a stale id is skipped instead of drawing
        // another mesh. Its geometry range does return to the pool.
        // Textures are never removed during a session.
        std::vector<Mesh_GPU>    meshes;
        std::vector<Texture_GPU> textures;

        // Released meshes whose geometry range may still be read by a frame
        // in flight. The range goes back to the pool once every frame
        // submitted before the release has completed.
        struct Retired_Mesh
        {
            uint32_t gpu_id = 0;
            uint64_t last_frame_serial = 0;   // submitted_frames at release time
        };

        std::vector<Retired_Mesh> retired_meshes;

        // Serial of the last frame submitted (0 before the first one), the
        // serial each frame slot submitted last, and the highest serial
        // known to be complete. A fence wait on a slot completes its
        // serial and, since fence signals include every earlier
        // submission, all the serials before it.
        uint64_t                                  submitted_frames = 0;
        std::array<uint64_t, FRAMES_IN_FLIGHT>    slot_frame_serial{};
        uint64_t                                  completed_frames = 0;

        // Dedicated transient command pool for transfer operations
        // (Upload_batch). Separate from the per-frame render command pools
        // so uploads don't interfere with frames in flight.
        Vulkan_Command_Pool transfer_command_pool;

        // Fence for the one-off transfer submissions above. Reused across
        // uploads (reset before each submit) rather than created and
        // destroyed per upload.
        VkFence transfer_fence = VK_NULL_HANDLE;

        // Set by Notify_framebuffer_resized, consumed by
        // Recreate_swapchain_if_needed.
        bool framebuffer_resized = false;

        // =========================================================
        // Internal helpers
        // =========================================================

        static Pipeline_Config Make_opaque_config();
        static Pipeline_Config Make_transparent_config();
        static Pipeline_Config Make_composite_config();

        // _wireframe: VK_POLYGON_MODE_LINE (needs fillModeNonSolid);
        // otherwise filled ellipsoids with alpha blending.
        static Pipeline_Config Make_bounds_config(bool _wireframe);

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
        // and the transfer fence exist.
        void Init_procedural_pass();

        // Creates material_buffer and mesh_table_buffer, allocates
        // per_material_set and writes both of its bindings, and registers
        // the default material, checking that it lands in slot
        // CoreTypes::Default_Material. Called once from the constructor,
        // after the descriptor pool exists, after Upload_default_textures
        // (the default material samples Default_Texture::White), before
        // any other material is registered and before any mesh upload
        // (uploads write the mesh table).
        void Init_global_tables();

        // Creates cluster_aabb_buffer and writes it into per_pass_set
        // (Binding_Per_Pass::Cluster_AABBs). Called once from the
        // constructor, after Init_procedural_pass allocated per_pass_set.
        void Init_light_clusters();

        // Uploads the unit sphere of the bounding volume debug view. Called
        // once from the constructor, after Init_global_tables.
        void Init_debug_meshes();

        // Names the resources created at startup (VK_EXT_debug_utils).
        void Name_debug_objects();

        // Names the OIT targets, which are recreated with the swapchain.
        void Name_oit_targets();

        // Returns to the pool the geometry of the released meshes whose
        // last possible reader (serial) has completed.
        void Free_retired_meshes();

        // Blocks until every submitted frame has completed (the fences of
        // all frame slots), then frees the retired geometry. Used before
        // an upload writes buffers that frames in flight read.
        void Wait_for_frames_in_flight();

        // Reads what the GPU measured for the last frame recorded in
        // _frame_slot (timestamps and counters), accumulates it and prints
        // it once per second when enabled. Called right after the fence of
        // the slot was waited on.
        void Read_frame_statistics(uint32_t _frame_slot);

        // Adds the scopes of timer_frame to statistics.scopes.
        void Accumulate_scope_timings();

        // Prints the average of every accumulated scope, the time outside
        // the top-level scopes and the frame total (print_stats).
        void Print_gpu_timings() const;

        // Forgets everything accumulated since the last print; the next
        // print covers only frames measured from now on.
        void Reset_statistics();

        // Frustum used for culling this frame: the packet's, or the frozen
        // one while Render_Debug_Settings::freeze_culling is set.
        void Update_culling_frustum(const CoreTypes::RenderPacket& _packet);

        std::vector<Pipeline_Config> Build_pipeline_manifest() const;

        // Creates one Image_Sync per swapchain image of the CURRENT swapchain.
        void Create_image_sync();

        // Moves the current Image_Sync objects out of service: the ones
        // whose present is known to have completed are destroyed at once,
        // the rest go to retired_sync.
        void Retire_image_sync();

        // Destroys retired synchronization objects whose grace period has
        // elapsed. _force (shutdown) waits for any pending present fence
        // and destroys everything.
        void Flush_retired_sync(bool _force);

        // Destroys every handle this class owns directly (not through a
        // member's destructor). Used by the destructor and by the
        // constructor's failure path.
        void Destroy_owned_handles();

        // Ends recording of _transfer_cmd, submits it signaling
        // transfer_fence, and blocks the CPU until that fence is signaled.
        // The CPU waits, so the primitive is a fence, not vkQueueWaitIdle,
        // which would also stall every frame already in flight on the
        // graphics queue.
        void Submit_and_wait_transfer(VkCommandBuffer _transfer_cmd);

        // Copies the packet's view and lights into the frame's mapped buffers.
        void Write_frame_uniforms(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet);

        // Records all render commands for one frame into the command
        // buffer of the given frame slot.
        void Record_command_buffer(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet, uint32_t _image_index);

        // Writes the object buffer of _frame and fills opaque_draws and
        // transparent_draws. Items whose pass_mask lacks the bit of their
        // list are skipped, and so are items that reference a pipeline,
        // mesh, transform or material that does not exist, a released
        // mesh, or a pipeline built for another subpass than the one their
        // list is drawn in (opaque items: Render_Subpass::Opaque,
        // transparent items: Render_Subpass::Transparent).
        //
        // Every kept item gets the next entry of the object buffer; the
        // entry index is the draw's firstInstance. Opaque items take
        // indices [0, opaque) and transparent items continue from there,
        // which is what lets the culling pass process [0, opaque) only.
        // Items beyond MAX_OBJECTS are skipped.
        //
        // _cull_transparents: transparent items whose bounding ellipsoid
        // (mesh bounding sphere placed by the model matrix) lies outside
        // culling_frustum get no entry (CPU culling, milestone 4.3). Same
        // test as cull_objects.comp: CoreTypes::Frustum::Intersects_ellipsoid.
        void Prepare_objects(Frame_Data& _frame, const CoreTypes::RenderPacket& _packet, bool _cull_transparents);

        // Opaque path that can actually be used for _packet: the requested
        // one, or Cpu_Indirect when a GPU path cannot represent the frame
        // (see Opaque_Draw_Path).
        Opaque_Draw_Path Resolve_opaque_path(Opaque_Draw_Path _requested, const CoreTypes::RenderPacket& _packet);

        // Writes one VkDrawIndexedIndirectCommand per opaque draw into the
        // host-visible command buffer of _frame, in opaque_draws order.
        void Write_cpu_draw_commands(Frame_Data& _frame) const;

        // Rebuilds the cluster boxes and records their upload when the
        // projection or the near plane changed since the last build.
        void Record_cluster_aabb_update(VkCommandBuffer _command_buffer, const CoreTypes::RenderPacket& _packet);

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

        // Recreates the swapchain, depth resources, OIT targets (and the
        // descriptors that read them), framebuffers and per-image
        // synchronization after a resize or OUT_OF_DATE error. Pipelines
        // are unaffected (viewport/scissor are dynamic).
        void Recreate_swapchain();

    public:

        // _validation_mode is forwarded to Vulkan_Instance; see
         // Validation_Mode for what each level checks and what it costs.
        Renderer(const Platform::Window& _window, Validation_Mode _validation_mode);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;
        Renderer(Renderer&&) = delete;
        Renderer& operator=(Renderer&&) = delete;

        // =========================================================
        // Asset upload
        // =========================================================

        // Uploads a mesh into the Geometry_Pool (vertices, 32-bit indices
        // and its mesh table entry, bounding sphere included) and returns
        // its gpu_id: the index into the Renderer's internal mesh registry,
        // which ResourceManager stores through Register_gpu_id() and
        // Draw_Item::mesh_gpu_id references.
        //
        // Throws std::invalid_argument if _mesh_data has no vertices or no
        // indices, and std::runtime_error if the mesh table (MAX_MESHES)
        // or the Geometry_Pool is full.
        uint32_t Upload_mesh(const CoreTypes::MeshData& _mesh_data);

        // Stops drawing mesh _gpu_id and returns its geometry to the pool
        // once no frame in flight can read it any more. The id is not
        // reused: Draw_Items that still reference it are skipped with a
        // warning. Releasing an id twice, or an id that was never handed
        // out, does nothing.
        void Release_mesh(uint32_t _gpu_id);

        // Uploads a texture (with a full mip chain) to the GPU, registers
        // it in the global bindless texture array, and returns its
        // BINDLESS INDEX: the value shaders use to index into the texture
        // array (set 3, binding 0). The texture carries no sampler: the
        // shader pairs it with the slot of the sampler array (set 3,
        // binding 1) that the material selects, e.g.:
        //   Sample_bindless(material.albedo_texture_index, material.albedo_sampler_index, uv)
        // where the index returned here reaches the shader as
        // Material_Desc::albedo_texture_index (see Register_material).
        //
        //
        // This is distinct from the internal Texture_GPU registry index
        // (used only to keep the Texture_GPU object alive); callers
        // (ResourceManager, Material loading) should only ever store and
        // use the bindless index returned here.
        //
        // _format: VK_FORMAT_R8G8B8A8_SRGB for color textures (albedo,
        //   emissive), VK_FORMAT_R8G8B8A8_UNORM for data textures (normal,
        //   metallic-roughness, AO). Caller decides based on texture role.
        uint32_t Upload_texture(const CoreTypes::ImageData& _image_data, VkFormat _format);

        // Same, with the format derived from ImageData::format.
        uint32_t Upload_texture(const CoreTypes::ImageData& _image_data);

        // Uploads every mesh and texture in _batch using ONE command buffer
        // and ONE queue submission, instead of one of each per asset.
        // Meshes are copied into the Geometry_Pool from one staging buffer
        // (one vkCmdCopyBuffer with a region per mesh for the vertices,
        // another for the indices, a third for their mesh table entries);
        // Texture_GPU only records into the command buffer it is handed and
        // never submits, which is what makes this possible.
        //
        // Ids come back in the same order as the input lists:
        //   result.mesh_gpu_ids[i]             <- _batch.meshes[i]
        //   result.texture_bindless_indices[i] <- _batch.textures[i]
        //
        // Upload_mesh and Upload_texture above are thin wrappers over this
        // function with a single-element batch: there is one upload path.
        //
        // Cost to be aware of: every staging buffer in the batch stays alive
        // until the submit completes, so peak host-visible memory is the SUM
        // of the batch, not the largest item. Split very large scenes into
        // several batches if that ever matters.
        //
        // Strong exception guarantee: if any upload fails, nothing of the
        // batch stays registered. Every check that can reject the batch
        // (null or empty data, a full bindless texture array, a full mesh
        // table or geometry pool) runs before anything is recorded; once
        // the submit has completed, registering the textures in the
        // bindless array cannot fail.
        //
        // Throws std::invalid_argument for a null or empty element, and
        // std::runtime_error if the bindless texture array does not have a
        // free slot for every texture of the batch, or the mesh table or
        // the geometry pool cannot hold its meshes.
        Upload_Batch_Result Upload_batch(const Upload_Batch& _batch);

        // =========================================================
        // Materials
        // =========================================================

        // Registers a material in the material table (set 2) and returns
        // its slot: the value Draw_Item::material_index carries and the
        // shaders index the table with. A description equal to one already
        // registered returns that slot and writes nothing.
        //
        // Slots are never released or rewritten, so the returned index is
        // valid for the whole lifetime of the Renderer. Meant for load
        // time; calling it between frames is also valid, because a new
        // slot is never referenced by a command already submitted.
        //
        // Throws std::invalid_argument if albedo_texture_index is not a
        // registered bindless slot or sampler is not a Sampler_Preset
        // value, and std::runtime_error if the table is full
        // (MAX_MATERIALS).
        uint32_t Register_material(const Material_Desc& _desc);

        // =========================================================
        // Compute pass outputs
        // =========================================================

        // Bindless index of the texture generated by procedural.comp, which
        // is regenerated only when its content changes (once, in the first
        // frame, today). Used like the index returned by Upload_texture:
        // stored as a material's albedo texture index and sampled with any
        // sampler preset. The image is UNORM, so it is read as linear color.
        // Valid for the whole lifetime of the Renderer.
        uint32_t Get_procedural_texture_index() const { return procedural_texture_index; }

        // =========================================================
        // Pipelines
        // =========================================================

        uint8_t Get_opaque_pipeline_id() const { return opaque_pipeline_id; }
        uint8_t Get_transparent_pipeline_id() const { return transparent_pipeline_id; }

        // =========================================================
        // Debug switches
        // =========================================================

        const Render_Debug_Settings& Get_debug_settings() const { return debug_settings; }

        // Applies new switches from the next frame on and logs every value
        // that changed. Out-of-range enumerators are replaced by their
        // defaults. Enabling freeze_culling freezes the frustum of the next
        // frame.
        void Set_debug_settings(const Render_Debug_Settings& _settings);

        // =========================================================
        // Surface size
        // =========================================================

        // Marks the swapchain as needing recreation. Called by the engine
        // loop when Window::Consume_resized_flag() reports a resize.
        void Notify_framebuffer_resized();

        // Rebuilds the swapchain if a resize was notified and the window
        // currently has a non-zero framebuffer. Returns true if the
        // swapchain is usable afterwards (false while minimized).
        // The engine loop calls it before extracting the frame, so the
        // aspect ratio and the viewport come from the same extent.
        bool Recreate_swapchain_if_needed();

        // Size of the images being rendered into: the swapchain extent.
        // This, not the live window size, is what the projection's aspect
        // ratio must be derived from, because it is what the viewport uses.
        void Get_render_size(uint32_t& _out_width, uint32_t& _out_height) const;

        // =========================================================
        // Render
        // =========================================================

        // Draws one frame from the given RenderPacket.
        // Handles frame-in-flight synchronization, command recording,
        // submission, and presentation internally.
        // On swapchain out-of-date (resize), recreates it and skips the frame.
        void Render(const CoreTypes::RenderPacket& _packet);
    };

} // namespace Renderer_System
