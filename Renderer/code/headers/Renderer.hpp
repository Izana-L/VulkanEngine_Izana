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
    // metallic-roughness, AO). ImageData itself doesn't know its own
    // color space.
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
        // Index into the Renderer's internal mesh registry — the value
        // ResourceManager stores through Register_gpu_id().
        std::vector<uint32_t> mesh_gpu_ids;

        // Index into the global bindless sampler array (set 1, binding 0)
        // — what shaders use, NOT the internal texture registry index.
        std::vector<uint32_t> texture_bindless_indices;
    };
    // Renderer: the only Vulkan-facing class that EngineCore knows about.
    //
    // Owns the entire Vulkan stack (instance → device → swapchain → pipeline)
    // and all per-frame resources. Exposes operations to EngineCore:
    //
    //   Upload_mesh()    — uploads geometry to the GPU once at load time.
    //   Upload_texture() — uploads a texture (with mipmaps) once at load time.
    //   Render()         — consumes a RenderPacket and produces one frame.
    //
    // Swapchain recreation on resize is handled internally — EngineCore
    // never sees it happen.
    //
    // Not copyable or movable: owns the entire Vulkan lifetime.
    class Renderer
    {
    private:

        // Vulkan core — construction order matters for destruction
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

        // Bindless registry must exist BEFORE the pipeline, since the
        // pipeline layout references bindless_registry.Get_layout() as
        // its set 1. Declared here (before pipeline) so construction
        // order is correct — C++ builds members in declaration order.
        Bindless_Registry      bindless_registry;
        // Must be declared BEFORE the pipeline: the pipeline constructor
        // takes its handle. Destruction runs in reverse, so the pipeline is
        // gone before the cache is serialized — which is what we want.
        Pipeline_Cache         pipeline_cache;
        // The layout every pipeline shares. Before the registry, because
        // pipelines are built against it and must be destroyed before it.
        Pipeline_Layout        pipeline_layout;

        // All pipelines, keyed by config. Replaces the single `pipeline`
        // member: a by-value member can only ever hold one.
        Pipeline_Registry      pipeline_registry;

        // The config for the one pipeline that exists today. Held as a
        // member so the per-frame lookup doesn't rebuild two std::strings
        // and hash them on every frame.
        Pipeline_Config        opaque_config;
        uint8_t                opaque_pipeline_id = 0;
        // Last bind count printed, so the log only speaks when it changes.
        // A member, not a function-level static: a static would be shared
        // by every Renderer in the process and is not reentrant.
        uint32_t               last_reported_binds = 0xFFFFFFFF;
        // A single value for now — there's one pipeline and one opaque batch.
        // Becomes per-batch once draws are sorted by pipeline.
        Raster_State           raster_state;
        // Shared sampler configurations, deduplicated.
        Sampler_Cache sampler_cache;

        // =========================================================
        // Frame resources
        // =========================================================

        static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

        std::array<Frame_Data, FRAMES_IN_FLIGHT> frames;
        uint32_t current_frame = 0;

        // =========================================================
        // Descriptors
        // =========================================================

        VkDescriptorPool                                  descriptor_pool;
        std::array<VkDescriptorSet, FRAMES_IN_FLIGHT>     descriptor_sets;

        // =========================================================
        // Asset registries
        // =========================================================

        // gpu_id = index into this vector.
        // Meshes/textures are never removed during a session (no gpu_id
        // recycling).
        std::vector<Mesh_GPU>    meshes;
        std::vector<Texture_GPU> textures;

        // Dedicated command pool for transfer operations (Upload_mesh,
        // Upload_texture). Separate from the per-frame render command
        // pools so uploads don't interfere with frames in flight.
        VkCommandPool transfer_command_pool;
        // Fence for the one-off transfer submissions above. Reused across
        // uploads (reset before each submit) rather than created and
        // destroyed per upload.
        VkFence transfer_fence;
        // =========================================================
        // Internal helpers
        // =========================================================

        void Init_descriptor_pool();
        void Init_descriptor_sets();
        std::vector<Pipeline_Config> Build_pipeline_manifest() const;
        // Ends recording of _transfer_cmd, submits it signaling
        // transfer_fence, and blocks the CPU until that fence is signaled.
        // The CPU waits, so the primitive is a fence — not vkQueueWaitIdle,
        // which would also stall every frame already in flight on the
        // graphics queue.
        void Submit_and_wait_transfer(VkCommandBuffer _transfer_cmd);
        // Records all render commands for one frame into the command
        // buffer of the current frame slot.
        void Record_command_buffer(const CoreTypes::RenderPacket& _packet,
            uint32_t                       _image_index);

        // Recreates the swapchain, depth resources, and framebuffers
        // after a resize or OUT_OF_DATE error. Pipeline is unaffected
        // (viewport/scissor are dynamic).
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
        // Thread-safety: not thread-safe — call from the main thread
        // during loading, not during rendering.
        uint32_t Upload_mesh(const CoreTypes::MeshData& _mesh_data);

        // Uploads a texture (with a full mip chain) to the GPU, registers
        // it in the global bindless descriptor array, and returns its
        // BINDLESS INDEX — the value shaders use to index into the
        // sampler array (set 1, binding 0), e.g.:
        //   texture(textures[nonuniformEXT(material.albedo_index)], uv)
        //
        // This is distinct from the internal Texture_GPU registry index
        // (used only to keep the Texture_GPU object alive) — callers
        // (ResourceManager, Material loading) should only ever store and
        // use the bindless index returned here.
        //
        // _format: VK_FORMAT_R8G8B8A8_SRGB for color textures (albedo,
        //   emissive), VK_FORMAT_R8G8B8A8_UNORM for data textures (normal,
        //   metallic-roughness, AO). Caller decides based on texture role.
        // Thread-safety: not thread-safe — call from the main thread
        // during loading, not during rendering.
        uint32_t Upload_texture(const CoreTypes::ImageData& _image_data, VkFormat _format);

        // Uploads every mesh and texture in _batch using ONE command buffer
        // and ONE queue submission, instead of one of each per asset.
        // Mesh_GPU and Texture_GPU only record into the command buffer they
        // are handed — they never submit — which is what makes this possible.
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
        // Thread-safety: not thread-safe — call from the main thread during
        // loading, not during rendering.
        Upload_Batch_Result Upload_batch(const Upload_Batch& _batch);


        uint8_t Get_opaque_pipeline_id() const { return opaque_pipeline_id; }
        // =========================================================
        // Render
        // =========================================================

        // Draws one frame from the given RenderPacket.
        // Handles frame-in-flight synchronization, command recording,
        // submission, and presentation internally.
        // On swapchain out-of-date (resize), recreates it and retries.
        void Render(const CoreTypes::RenderPacket& _packet);

   
    };

} // namespace Renderer