#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <cstdint>
#include <deque>
#include <vector>

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
    // verifies it (Vulkan_Device::Is_bindless_supported) and throws
    // std::runtime_error otherwise. The sampled image features already in
    // use cover SAMPLER and SAMPLED_IMAGE as well as
    // COMBINED_IMAGE_SAMPLER, so the split needs no new feature.
    //
    // Texture slots are not permanent. Each one goes through:
    //   Register_texture - a view is written into a free slot and its
    //                      index is returned.
    //   Update_texture   - the slot is rewritten with a new view and keeps
    //                      its index. Meant for images recreated with a new
    //                      view, such as a resolution-dependent
    //                      Storage_Image after a resize: every draw keeps
    //                      its index and reads the new view, and no extra
    //                      slot is consumed.
    //   Release_texture  - the slot is returned for reuse once its image
    //                      is no longer needed.
    // Slots 0 .. CoreTypes::Default_Texture::Count - 1 hold the default
    // textures, which the Renderer registers before anything else. Every
    // draw item relies on them, so they are reserved: they can be neither
    // rewritten nor released.
    //
    // Thread-safety: none of the functions that write the set is
    // thread-safe; they are called from the main thread, like
    // Renderer::Upload_texture and Renderer::Upload_mesh.
    //
    // Lifetime: owned by Renderer, constructed once at startup after the
    // device confirms bindless support, destroyed at shutdown.
    class Bindless_Registry
    {
    public:

        // _desired_textures: requested size of the texture array (binding 0).
        // _desired_samplers: requested size of the sampler array (binding 1).
        // The effective sizes are the requests clamped to the device
        // limits (Vulkan_Device::Get_bindless_limits); both are logged at
        // startup. They are fixed at layout creation even though most
        // slots start empty (VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
        // allows that, as long as the shader never reads an empty slot).
        // No default values: the Renderer passes its named constants
        // (BINDLESS_DESIRED_TEXTURES, BINDLESS_DESIRED_SAMPLERS).
        //
        // Throws std::invalid_argument if a request is zero, and
        // std::runtime_error if the device cannot offer one sampler slot
        // per CoreTypes::Sampler_Preset or a minimum of texture slots.
        explicit Bindless_Registry(const Vulkan_Device& _device,
            uint32_t              _desired_textures,
            uint32_t              _desired_samplers);

        ~Bindless_Registry();

        Bindless_Registry(const Bindless_Registry&) = delete;
        Bindless_Registry& operator=(const Bindless_Registry&) = delete;
        Bindless_Registry(Bindless_Registry&&) = delete;
        Bindless_Registry& operator=(Bindless_Registry&&) = delete;

        // Writes _image_view into a free slot of the texture array and
        // returns its index. This index is what gets stored in
        // Material_GPU and ultimately read by the shader. No sampler is
        // involved: the shader pairs the texture with a slot of the
        // sampler array at the point of use.
        //
        // Slot choice: slots that were never used are taken first, in
        // increasing order, so the first registrations of a session (the
        // default textures) land in slots 0, 1, 2 and so on. Once they run
        // out, released slots are reused in the order they were released,
        // which keeps a stale index reading the error texture for as long
        // as possible (see Release_texture). No command buffer pending
        // execution reads either kind of slot, so the write needs no wait.
        //
        // Declared layout: the descriptor of every slot records
        // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL. The following rule
        // keeps that valid for images the GPU rewrites (compute outputs,
        // render targets), not only for uploaded textures:
        //
        //   While any draw or dispatch that may read a slot can execute,
        //   the image of that slot is in its declared layout and its last
        //   writes are visible to Bindless_Reader_Pipeline_Stages
        //   (Shader_Stages.hpp). Whoever writes a registered image takes it
        //   out of the declared layout and returns it there before any
        //   reader executes.
        //
        // With PARTIALLY_BOUND only the slots actually read matter, but no
        // command can prove which slots it reads, so any command that may
        // index the arrays counts as a reader of every slot.
        //
        // How each kind of writer complies:
        //   uploads (Texture_GPU)  - written once; the upload barriers leave
        //                            the image in the declared layout and it
        //                            never leaves it again.
        //   compute outputs        - created with a clear that ends in the
        //   (Storage_Image)          declared layout, so the slot is valid
        //                            before the first write; then
        //                            UNDEFINED -> GENERAL before each write
        //                            and GENERAL -> SHADER_READ_ONLY_OPTIMAL
        //                            after it (Begin_write / End_write), all
        //                            recorded before vkCmdBeginRenderPass.
        //   render passes (future) - finalLayout SHADER_READ_ONLY_OPTIMAL
        //                            plus external subpass dependencies from
        //                            and towards the reader stages; no
        //                            explicit barrier.
        //
        // Policy: a single declared layout for every slot, so all bindless
        // reads use SHADER_READ_ONLY_OPTIMAL and this function takes no
        // layout. The alternative, keeping written images permanently in
        // GENERAL, would add a layout parameter here and can disable image
        // compression on some hardware.
        //
        // Throws std::invalid_argument if _image_view is VK_NULL_HANDLE,
        // and std::runtime_error if no slot is free (every slot is in use
        // and none has been released).
        uint32_t Register_texture(VkImageView _image_view);

        // Rewrites registered slot _index with _image_view; the index stays
        // the same. The image of the new view follows the declared layout
        // rule above like any registered image. Once this returns, the slot
        // no longer references the previous view, so the registry places
        // no constraint on destroying it.
        //
        // Precondition: no command buffer pending execution reads the slot.
        // Frames still executing would otherwise have a descriptor they use
        // replaced under them: UPDATE_AFTER_BIND only covers command
        // buffers that have not been submitted yet, and
        // UPDATE_UNUSED_WHILE_PENDING only descriptors they do not use.
        // It holds after vkDeviceWaitIdle, which Renderer::Recreate_swapchain
        // performs before resolution-dependent images are recreated, or
        // once the fence of every frame in flight that may read the slot
        // has signaled.
        //
        // Throws std::invalid_argument if _image_view is VK_NULL_HANDLE, if
        // _index is not a registered slot, or if it is a reserved default
        // texture slot.
        void Update_texture(uint32_t _index, VkImageView _image_view);

        // Returns registered slot _index for reuse. The slot is rewritten
        // with the view of CoreTypes::Default_Texture::Error, so a draw that
        // still carries the index reads the magenta error texture instead
        // of a view that may already be destroyed; once this returns, the
        // registry places no constraint on destroying the released view.
        // A later Register_texture reuses the slot (see there for the
        // order), from which point a stale index reads that other texture:
        // an index must not be used once released.
        //
        // Precondition: the same as Update_texture, no command buffer
        // pending execution reads the slot. It also keeps the slot safe to
        // reuse later: no command recorded after this call may use the
        // released index, so it stays unread by every frame in flight.
        //
        // Throws std::invalid_argument if _index is not a registered slot
        // or if it is a reserved default texture slot.
        void Release_texture(uint32_t _index);

        // True when slot _index holds a registered texture: it was written
        // by Register_texture and has not been released since.
        bool Is_texture_registered(uint32_t _index) const;

        // Writes _sampler into slot _index of the sampler array. Slots are
        // fixed, not allocated: slot i holds the sampler of
        // CoreTypes::Sampler_Preset value i.
        //
        // Rewriting a slot is legal only while no command buffer pending
        // execution reads it: UPDATE_AFTER_BIND covers a command buffer
        // that bound the set and is still being recorded, and
        // UPDATE_UNUSED_WHILE_PENDING only descriptors that pending command
        // buffers do not read. Today it is only called at startup, before
        // the first frame is recorded.
        //
        // Throws std::invalid_argument if _sampler is VK_NULL_HANDLE or
        // _index is not below Get_max_samplers().
        void Set_sampler(uint32_t _index, VkSampler _sampler);

        // Returns how many texture slots hold a registered texture
        // (registered and not released).
        uint32_t Get_registered_count() const;

        // Returns how many more textures Register_texture can take before
        // it throws: the slots never used plus the released ones. A caller
        // that registers several textures after work it cannot undo (the
        // upload batch in Renderer::Upload_batch) checks it first; with
        // enough free slots and non-null views, Register_texture does not
        // throw, because storage for every slot is reserved at
        // construction.
        uint32_t Get_free_texture_count() const;

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

        // Writes _image_view into slot _index of the texture array.
        void Write_texture_slot(uint32_t _index, VkImageView _image_view);

        // Throws std::invalid_argument unless _index is a registered slot
        // that may be rewritten or released, i.e. not a default texture
        // slot. _caller names the public function in the message.
        void Validate_mutable_slot(uint32_t _index, const char* _caller) const;

        VkDevice device_handle;
        uint32_t max_textures;
        uint32_t max_samplers;

        // View written in every slot used so far, by index; VK_NULL_HANDLE
        // marks a released slot. Its size is the number of slots ever used,
        // so slot_views.size() is the next slot that was never used. Its
        // capacity is reserved for max_textures at construction, so
        // push_back never reallocates and never throws.
        std::vector<VkImageView> slot_views;

        // Released slots, oldest first. Taken by Register_texture only once
        // every never-used slot has been used.
        std::deque<uint32_t> released_slots;

        VkDescriptorSetLayout layout;
        VkDescriptorPool      pool;
        VkDescriptorSet       set;
    };

} // namespace Renderer