#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <cstdint>

namespace Renderer
{

    // Bindless_Registry: owns the single global descriptor set that holds
    // every texture the engine has uploaded, as one large sampler array.
    //
    // Replaces the classic "bind a descriptor set per material before each
    // draw" model. Instead, this set is bound ONCE per frame, and each
    // draw item carries texture indices (via push constant or material
    // buffer) that the shader uses to index directly into the array:
    //
    //   layout(set = 1, binding = 0) uniform sampler2D textures[];
    //   vec4 albedo = texture(textures[nonuniformEXT(material.albedo_index)], uv);
    //
    // Requires the device to support descriptor indexing features
    // (Vulkan_Device::Is_bindless_supported() must be true before
    // constructing this — checked via assert).
    //
    // Index allocation is sequential and permanent: textures are never
    // unregistered or have their slots reused during a session, mirroring
    // how Renderer assigns mesh/texture gpu_ids.
    //
    // Lifetime: owned by Renderer, constructed once at startup after the
    // device confirms bindless support, destroyed at shutdown.
    class Bindless_Registry
    {
    public:

        // _max_textures: fixed size of the descriptor array. Must be known
        // at layout creation time even though most slots start empty
        // (VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT allows that). 1024 is
        // generous for a single-scene development workload; raise if a
        // scene's unique texture count approaches this limit.
        explicit Bindless_Registry(const Vulkan_Device& _device,
            uint32_t              _max_textures = 1024);

        ~Bindless_Registry();

        Bindless_Registry(const Bindless_Registry&) = delete;
        Bindless_Registry& operator=(const Bindless_Registry&) = delete;
        Bindless_Registry(Bindless_Registry&&) = delete;
        Bindless_Registry& operator=(Bindless_Registry&&) = delete;

        // Writes {_image_view, _sampler} into the next free slot of the
        // global array and returns its index. This index is what gets
        // stored in Material_GPU and ultimately read by the shader.
        //
        // Thread-safety: not thread-safe — call from the main thread
        // during loading, same as Renderer::Upload_texture/Upload_mesh.
        //
        // Throws if _max_textures slots are already in use.
        uint32_t Register_texture(VkImageView _image_view, VkSampler _sampler);

        // Returns how many texture slots have been registered so far.
        uint32_t Get_registered_count() const;

        // =========================================================
        // Getters — needed by Vulkan_Pipeline (layout) and Renderer
        // (binding the set once per frame)
        // =========================================================

        VkDescriptorSetLayout Get_layout() const;
        VkDescriptorSet        Get_set()    const;

    private:

        void Create_layout();
        void Create_pool();
        void Create_set();

        VkDevice device_handle;
        uint32_t max_textures;
        uint32_t next_free_index;

        VkDescriptorSetLayout layout;
        VkDescriptorPool      pool;
        VkDescriptorSet       set;
    };

} // namespace Renderer