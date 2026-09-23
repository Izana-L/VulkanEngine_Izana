#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <cstdint>

namespace Renderer_System
{

    // Bindless_Registry: owns the single global descriptor set (set 3)
    // that holds every texture the engine has uploaded and every sampler
    // those textures are read with, as two arrays:
    //   binding 0 (Binding_Bindless::Textures) - SAMPLED_IMAGE array, one
    //             slot per texture, allocated by Register_texture.
    //   binding 1 (Binding_Bindless::Samplers) - small SAMPLER array, one
    //             fixed slot per CoreTypes::Sampler_Preset, written by
    //             Set_sampler.
    //
    // Image and sampler are kept apart so the filtering is chosen by the
    // material, not by the texture: each texture occupies a single slot
    // whatever sampler reads it, and the shader combines both at the point
    // of use.
    //
    // Replaces the classic "bind a descriptor set per material before each
    // draw" model. Instead, this set is bound ONCE per frame, and each
    // draw item carries a texture index and a sampler index (via push
    // constant or material buffer) that the shader uses to index directly
    // into the arrays:
    //
    //   layout(set = 3, binding = 0) uniform texture2D textures[];
    //   layout(set = 3, binding = 1) uniform sampler   samplers[];
    //   vec4 albedo = texture(sampler2D(textures[nonuniformEXT(tex)],
    //                                   samplers[nonuniformEXT(smp)]), uv);
    //
    // Requires the device to support descriptor indexing features.
    // Vulkan_Device only selects devices that do; the constructor still
    // verifies it and throws std::runtime_error otherwise. The sampled
    // image features already in use cover SAMPLER and SAMPLED_IMAGE as
    // well as COMBINED_IMAGE_SAMPLER, so the split needs no new feature.
    //
    // Texture index allocation is sequential and permanent: textures are
    // never unregistered or have their slots reused during a session,
    // mirroring how Renderer assigns mesh/texture gpu_ids.
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
        explicit Bindless_Registry(const Vulkan_Device& _device,uint32_t  _max_textures, uint32_t   _max_samplers);

        ~Bindless_Registry();

        Bindless_Registry(const Bindless_Registry&) = delete;
        Bindless_Registry& operator=(const Bindless_Registry&) = delete;
        Bindless_Registry(Bindless_Registry&&) = delete;
        Bindless_Registry& operator=(Bindless_Registry&&) = delete;

        // Writes _image_view into the next free slot of the texture array
        // and returns its index. This index is what gets stored in
        // Material_GPU and ultimately read by the shader. No sampler is
        // involved: the shader pairs the texture with a slot of the
        // sampler array at the point of use.
        //
        // The descriptor records VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        // the image must be in that layout whenever a shader reads it.
        //
        // Thread-safety: not thread-safe — call from the main thread
        // during loading, same as Renderer::Upload_texture/Upload_mesh.
        //
        // Throws if _max_textures slots are already in use.
        uint32_t Register_texture(VkImageView _image_view);

        // Writes _sampler into slot _index of the sampler array. Slots are
        // fixed, not allocated: slot i holds the sampler of
        // CoreTypes::Sampler_Preset value i.
        //
        // Rewriting a slot is legal only while no submitted frame that is
        // still executing reads it: UPDATE_AFTER_BIND allows writing after
        // the set was bound in a command buffer being recorded, not while
        // the GPU may be using the descriptor. Today it is only called at
        // startup, before the first frame is recorded.
        //
        // Throws std::invalid_argument if _sampler is VK_NULL_HANDLE or
        // _index is not below Get_max_samplers().
        void Set_sampler(uint32_t _index, VkSampler _sampler);

        // Returns how many texture slots have been registered so far.
        uint32_t Get_registered_count() const;

        // Size of the sampler array: valid Set_sampler indices are
        // [0, Get_max_samplers()).
        uint32_t Get_max_samplers() const;

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
        uint32_t max_samplers;
        uint32_t next_free_index;

        VkDescriptorSetLayout layout;
        VkDescriptorPool      pool;
        VkDescriptorSet       set;
    };

} // namespace Renderer