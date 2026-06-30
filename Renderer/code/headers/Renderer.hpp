#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Instance.hpp>
#include <Vulkan_Surface.hpp>
#include <Vulkan_Device.hpp>
#include <Vulkan_Swapchain.hpp>
#include <Vulkan_Render_Pass.hpp>
#include <Vulkan_Depth_Resources.hpp>
#include <Vulkan_Framebuffer.hpp>
#include <Vulkan_Pipeline.hpp>
#include <Frame_Data.hpp>
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

namespace Renderer
{

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

        // =========================================================
        // Render
        // =========================================================

        // Draws one frame from the given RenderPacket.
        // Handles frame-in-flight synchronization, command recording,
        // submission, and presentation internally.
        // On swapchain out-of-date (resize), recreates it and retries.
        void Render(const CoreTypes::RenderPacket& _packet);

    private:

        // Vulkan core — construction order matters for destruction
        Vulkan_Instance        instance;
        Vulkan_Surface         surface;
        Vulkan_Device          device;
        Vulkan_Swapchain       swapchain;
        Vulkan_Render_Pass     render_pass;
        Vulkan_Depth_Resources depth_resources;
        Vulkan_Framebuffer     framebuffers;

        // Bindless registry must exist BEFORE the pipeline, since the
        // pipeline layout references bindless_registry.Get_layout() as
        // its set 1. Declared here (before pipeline) so construction
        // order is correct — C++ builds members in declaration order.
        Bindless_Registry      bindless_registry;

        Vulkan_Pipeline        pipeline;

        // Shared sampler configurations, deduplicated.
        Sampler_Cache sampler_cache;

        // =========================================================
        // Frame resources
        // =========================================================

        static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

        std::array<Frame_Data, FRAMES_IN_FLIGHT> frames;
        uint32_t                                 current_frame = 0;

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

        // =========================================================
        // Internal helpers
        // =========================================================

        void Init_descriptor_pool();
        void Init_descriptor_sets();

        // Records all render commands for one frame into the command
        // buffer of the current frame slot.
        void Record_command_buffer(const CoreTypes::RenderPacket& _packet,
            uint32_t                       _image_index);

        // Recreates the swapchain, depth resources, and framebuffers
        // after a resize or OUT_OF_DATE error. Pipeline is unaffected
        // (viewport/scissor are dynamic).
        void Recreate_swapchain();
    };

} // namespace Renderer