#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Shader_Stages.hpp>

#include <vk_mem_alloc.h>

#include <cstdint>

namespace Renderer_System
{

    // Storage_Image: a 2D image written by compute shaders and read by any
    // shader through the bindless set. Owns the VkImage, its VMA
    // allocation and a single VkImageView, used both as the storage image
    // a compute pass writes (layout GENERAL) and as the sampled image a
    // bindless slot reads (layout SHADER_READ_ONLY_OPTIMAL).
    //
    // Knows nothing about the bindless set: the owner registers
    // Get_image_view() through Bindless_Registry::Register_texture and
    // keeps the slot. The image follows the declared layout rule
    // documented there from the moment it exists:
    //
    //   constructor         // UNDEFINED -> TRANSFER_DST_OPTIMAL, clear to
    //                       // zero, -> SHADER_READ_ONLY_OPTIMAL
    //   Begin_write(cmd);   // UNDEFINED -> GENERAL
    //   ... dispatch that writes every texel ...
    //   End_write(cmd);     // GENERAL -> SHADER_READ_ONLY_OPTIMAL
    //
    // The initial clear is recorded by the constructor, so no image exists
    // whose slot could be read before it was brought to its declared
    // layout: registering the view and drawing before the first write is
    // valid and reads zeros.
    //
    // Rules for the owner:
    //   - The command buffer given to the constructor (or to Recreate) is
    //     submitted before, or is itself, the first one that may read the
    //     image's bindless slot, like the upload of a Texture_GPU.
    //   - The initial clear, Begin_write and End_write are recorded outside
    //     a render pass instance, before vkCmdBeginRenderPass: clears are
    //     not allowed inside one, and a barrier inside the current render
    //     pass would need a subpass self-dependency that the render pass
    //     does not declare.
    //   - Every Begin_write is followed by End_write in the same command
    //     buffer, before any command that may read the bindless set.
    //   - Begin_write discards the contents, so the dispatch writes every
    //     texel. A chain of dispatches in which one reads what an earlier
    //     one wrote (imageLoad in GENERAL) records its own GENERAL ->
    //     GENERAL barrier between them, with
    //     Vulkan_Image_Utils::Record_image_barrier (compute shader write ->
    //     compute shader read).
    //
    // Synchronization is explicit, never derived from the layouts: the
    // writer is the compute shader stage, and the readers are the stages
    // given to the constructor (see there), so the barriers stay valid on
    // a queue family without graphics support.
    //
    // Resolution-dependent images are recreated after a resize with
    // Recreate(), and the owner then rewrites the slot it already holds
    // with Bindless_Registry::Update_texture: the index stays the same and
    // no slot is consumed per resize.
    //
    // The layout is tracked on the CPU in recording order, only to assert
    // that Begin_write and End_write alternate; it is not the layout the
    // GPU sees at any given moment.
    //
    // One mip level: a compute write produces a single level, and a mip
    // chain would need a separate generation pass.
    //
    // Not copyable; movable, like Vulkan_Depth_Resources.
    class Storage_Image
    {
    public:
        // Creates a _width x _height image of _format with usage
        // STORAGE | SAMPLED | TRANSFER_DST, optimal tiling, and its view,
        // and records into _command_buffer the initial clear that leaves
        // every texel at zero (black and transparent for color formats) in
        // SHADER_READ_ONLY_OPTIMAL, visible to _reader_stages. Recorded,
        // not submitted: _command_buffer must be in the recording state,
        // outside a render pass instance.
        //
        // _reader_stages: the pipeline stages that read the image through
        // its bindless slot, as seen from the queue family of the command
        // buffers the barriers of this image are recorded in. Every stage
        // must be supported by that family. The default,
        // Bindless_Reader_Pipeline_Stages (FRAGMENT | COMPUTE), suits the
        // graphics family, which every command pool of the engine uses. A
        // compute-only queue passes VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        // readers on another queue are then ordered by semaphores, plus a
        // queue family ownership transfer when the families differ, neither
        // of which this class records.
        //
        // Throws std::invalid_argument if a dimension is zero, or if
        // _reader_stages is empty or holds a stage outside
        // Bindless_Reader_Pipeline_Stages (only those stages can read a
        // bindless slot). Throws std::runtime_error if the device does not
        // support _format with optimal tiling as a storage image, a sampled
        // image and a transfer destination. VK_FORMAT_R8G8B8A8_UNORM is
        // guaranteed by the specification to support all three; sRGB
        // formats rarely support storage.
        Storage_Image(const Vulkan_Device& _device,
            VmaAllocator         _allocator,
            VkCommandBuffer      _command_buffer,
            uint32_t             _width,
            uint32_t             _height,
            VkFormat             _format,
            VkPipelineStageFlags _reader_stages = Bindless_Reader_Pipeline_Stages);

        ~Storage_Image();

        Storage_Image(const Storage_Image&) = delete;
        Storage_Image& operator=(const Storage_Image&) = delete;

        Storage_Image(Storage_Image&& _other) noexcept;
        Storage_Image& operator=(Storage_Image&& _other) noexcept;

        // Replaces the image and its view with new ones of _new_extent
        // (same format and reader stages) and records their initial clear
        // into _command_buffer, exactly as the constructor does. Meant for
        // resolution-dependent outputs after a resize. The view handle
        // changes: the owner rewrites the image's bindless slot with
        // Bindless_Registry::Update_texture(slot, Get_image_view()).
        //
        // Precondition: no command buffer that is pending execution, or
        // recorded and still to be submitted, uses the old image or view,
        // which are destroyed here. It holds after vkDeviceWaitIdle and
        // between frames, as in Renderer::Recreate_swapchain.
        //
        // Throws std::invalid_argument if a dimension of _new_extent is
        // zero. If creating the new image fails, the exception propagates
        // and the old image, its view and its slot stay valid.
        void Recreate(VkCommandBuffer _command_buffer, VkExtent2D _new_extent);

        // Records UNDEFINED -> GENERAL through
        // Vulkan_Image_Utils::Record_image_barrier: waits for the reader
        // stages of earlier frames, then allows compute shader writes.
        // Expects the image back in its declared layout, after the initial
        // clear or after the previous End_write.
        void Begin_write(VkCommandBuffer _command_buffer);

        // Records GENERAL -> SHADER_READ_ONLY_OPTIMAL: makes the compute
        // writes visible to the reader stages and returns the image to its
        // declared layout. Expects a Begin_write before it.
        void End_write(VkCommandBuffer _command_buffer);

        VkImage              Get_image() const;
        VkImageView          Get_image_view() const;
        VkFormat             Get_format() const;
        VkExtent2D           Get_extent() const;
        VkPipelineStageFlags Get_reader_stages() const;

        // Layout left by the last recorded operation: the initial clear or
        // End_write leave SHADER_READ_ONLY_OPTIMAL, Begin_write leaves
        // GENERAL. UNDEFINED only for a moved-from object. Recording
        // order, not execution order.
        VkImageLayout Get_recorded_layout() const;

    private:
        // Creates an image of _extent with its view into the output
        // parameters. On failure, destroys whatever it created and
        // rethrows, leaving the outputs untouched.
        void Create_image_and_view(VkExtent2D _extent,
            Vulkan_Image_Utils::Image_Allocation& _out_image,
            VkImageView&                          _out_view) const;

        // Records the initial clear of the current image into
        // _command_buffer: UNDEFINED -> TRANSFER_DST_OPTIMAL, clear to
        // zero, TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL.
        void Record_initial_clear(VkCommandBuffer _command_buffer);

        // Destroys the view, then the image and its allocation. Safe on a
        // partially created or moved-from object. Shared by the
        // destructor, move assignment, Recreate and the constructor's
        // failure path.
        void Destroy();

        VkDevice             device_handle;
        VmaAllocator         allocator;
        VkFormat             format;
        VkExtent2D           extent;
        VkPipelineStageFlags reader_stages;

        Vulkan_Image_Utils::Image_Allocation image;
        VkImageView                          image_view;

        VkImageLayout recorded_layout;
    };

}
