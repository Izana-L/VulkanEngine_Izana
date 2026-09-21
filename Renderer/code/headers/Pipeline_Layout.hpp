#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Descriptor_Layout_Cache.hpp>
#include <Vulkan_Device.hpp>

namespace Renderer_System
{

    // Pipeline_Layout: the descriptor set layout (set 0) and the
    // VkPipelineLayout that EVERY pipeline in this engine shares.
    //
    // Both used to live inside Vulkan_Pipeline, which was correct while
    // there was exactly one pipeline. With a registry building N of them
    // that would mean N identical copies of both objects, so they are
    // hoisted here and created once.
    //
    // The layout contract, shared by every pipeline:
    //   set 0, binding 0 : per-frame view/projection UBO (vertex stage)
    //   set 1            : global bindless texture array (Bindless_Registry)
    //   push constant    : mat4 model, 64 bytes, vertex stage
    //
    // The day a pipeline needs a different contract (a compute pass, a
    // shadow pass with no bindless set), this stops being one shared object
    // and becomes a small cache keyed by the contract — same shape as
    // Pipeline_Registry. Not today: every pipeline here is the same shape.
    class Pipeline_Layout
    {
        VkDevice         device_handle;
        VkPipelineLayout pipeline_layout;              // ya no posee layouts de set
    public:
        // Los layouts de conjunto son ahora de Descriptor_Layout_Cache.
        // Esta clase solo compone el VkPipelineLayout a partir de los cuatro
        // y anade el rango de push constants.
        Pipeline_Layout(const Vulkan_Device& _device, const Descriptor_Layout_Cache& _layouts);
        VkPipelineLayout Get_handle() const { return pipeline_layout; }
        // Get_descriptor_set_layout() desaparece: quien necesite un layout de
        // set se lo pide a la cache, que es quien los tiene.
    };

} // namespace Renderer_System