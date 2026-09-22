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
#include <Vulkan_Framebuffer.hpp>
#include <Vulkan_Pipeline.hpp>
#include <Vulkan_Command_Pool.hpp>
#include <Frame_Data.hpp>
#include <Pipeline_Cache.hpp>
#include <Pipeline_Registry.hpp>
#include <Pipeline_Layout.hpp>
#include <Mesh_GPU.hpp>
#include <Texture_GPU.hpp>
#include <Sampler_Cache.hpp>
#include <Bindless_Registry.hpp>
#include <RenderPacket.hpp>
#include <ImageData.hpp>

#include <array>
#include <cstdint>
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

        // Index into the global bindless sampler array (set 3, binding 0):
        // what shaders use, NOT the internal texture registry index.
        std::vector<uint32_t> texture_bindless_indices;
    };

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

        Vulkan_Swapchain       swapchain;
        Vulkan_Render_Pass     render_pass;
        Vulkan_Depth_Resources depth_resources;
        Vulkan_Framebuffer     framebuffers;

        // Bindless registry must exist BEFORE the pipeline layout, which
        // references bindless_registry.Get_layout() as its set 3.
        Bindless_Registry      bindless_registry;
        // Must be declared BEFORE the pipelines: they are created through
        // the cache. Destruction runs in reverse, so the pipelines are gone
        // before the cache is serialized.
        Pipeline_Cache         pipeline_cache;
        Descriptor_Layout_Cache descriptor_layouts;
        // The layout every pipeline shares. Before the registry, because
        // pipelines are built against it and must be destroyed before it.
        Pipeline_Layout        pipeline_layout;

        // All pipelines, keyed by config.
        Pipeline_Registry      pipeline_registry;

        // The two pipelines that exist today: opaque (no blending, depth
        // write) and transparent (alpha blending, depth test only). Held
        // as members so the per-frame lookup never rebuilds and hashes
        // two std::strings.
        Pipeline_Config        opaque_config;
        Pipeline_Config        transparent_config;
        uint8_t                opaque_pipeline_id = 0;
        uint8_t                transparent_pipeline_id = 0;

        // Last bind count printed, so the log only speaks when it changes.
        // A member, not a function-level static: a static would be shared
        // by every Renderer in the process and is not reentrant.
        uint32_t               last_reported_binds = 0xFFFFFFFF;

        // Draw items that reference a mesh, transform or pipeline that
        // does not exist are skipped; the first one is reported once.
        bool                   warned_invalid_item = false;

        // Rasterization state shared by every batch; the transparent pass
        // only overrides depth writes.
        Raster_State           raster_state;

        // Shared sampler configurations, deduplicated.
        Sampler_Cache          sampler_cache;

        // =========================================================
        // Frame resources
        // =========================================================

        static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

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

        // =========================================================
        // Asset registries
        // =========================================================

        // gpu_id = index into this vector.
        // Meshes/textures are never removed during a session (no gpu_id
        // recycling).
        std::vector<Mesh_GPU>    meshes;
        std::vector<Texture_GPU> textures;

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

        void Init_descriptor_pool();
        void Init_descriptor_sets();
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
        void Record_command_buffer(Frame_Data& _frame,
            const CoreTypes::RenderPacket& _packet,
            uint32_t                       _image_index);

        // Records the draws of one item list for one pass. Items whose
        // pass_mask lacks _pass_bit are skipped.
        void Draw_items(VkCommandBuffer _command_buffer,
            const std::vector<CoreTypes::Draw_Item>& _items,
            uint8_t _pass_bit,
            const CoreTypes::RenderPacket& _packet,
            uint8_t& _bound_pipeline_id,
            uint32_t& _bind_count);

        // Recreates the swapchain, depth resources, framebuffers and
        // per-image synchronization after a resize or OUT_OF_DATE error.
        // Pipelines are unaffected (viewport/scissor are dynamic).
        void Recreate_swapchain();

    public:

        Renderer(const Platform::Window& _window, bool _enable_validation);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;
        Renderer(Renderer&&) = delete;
        Renderer& operator=(Renderer&&) = delete;

        // =========================================================
        // Asset upload
        // =========================================================

        // Uploads a mesh to the GPU and returns its gpu_id.
        // The gpu_id is an index into the internal mesh registry and
        // must be stored by the ResourceManager as the resolution of
        // the corresponding Asset_Handle.
        // Thread-safety: not thread-safe; call from the main thread
        // during loading, not during rendering.
        uint32_t Upload_mesh(const CoreTypes::MeshData& _mesh_data);

        // Uploads a texture (with a full mip chain) to the GPU, registers
        // it in the global bindless descriptor array, and returns its
        // BINDLESS INDEX: the value shaders use to index into the
        // sampler array (set 3, binding 0), e.g.:
        //   texture(textures[nonuniformEXT(push.albedo_texture_index)], uv)
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
        // Mesh_GPU and Texture_GPU only record into the command buffer they
        // are handed; they never submit, which is what makes this possible.
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
        // batch stays registered.
        Upload_Batch_Result Upload_batch(const Upload_Batch& _batch);

        // =========================================================
        // Pipelines
        // =========================================================

        uint8_t Get_opaque_pipeline_id() const { return opaque_pipeline_id; }
        uint8_t Get_transparent_pipeline_id() const { return transparent_pipeline_id; }

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
