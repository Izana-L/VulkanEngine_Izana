#include <Pipeline_Layout.hpp>
#include <Vulkan_Utils.hpp>
#include <Frame_Data.hpp>

#include <cassert>
#include <stdexcept>
#include <vector>

namespace Renderer_System
{

    Pipeline_Layout::Pipeline_Layout(const Vulkan_Device& _device,
        const Descriptor_Layout_Cache& _layouts)
        : device_handle(_device.Get_logical_device_handle()),
        pipeline_layout(VK_NULL_HANDLE)
    {
        // One range covering the whole Push_Constants block for both
        // stages: the vertex shader reads the model matrix, the fragment
        // shader reads the base color and the texture index. The size is
        // taken from the C++ mirror so the two cannot drift apart.
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(Push_Constants);

        static_assert(sizeof(Push_Constants) <= 128,
            "Push_Constants exceeds the 128-byte minimum maxPushConstantsSize");

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        // Always Descriptor_Set::Count. No conditionals: if a set is empty,
        // its empty layout still occupies the slot, so a set's number never
        // depends on whether another set exists.
        layout_info.setLayoutCount = Descriptor_Set::Count;
        layout_info.pSetLayouts = _layouts.Data();
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;

        VK_CHECK(vkCreatePipelineLayout(device_handle, &layout_info, nullptr, &pipeline_layout),
            "Failed to create pipeline layout");
    }

    Pipeline_Layout::~Pipeline_Layout()
    {
        if (pipeline_layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device_handle, pipeline_layout, nullptr);
            pipeline_layout = VK_NULL_HANDLE;
        }
    }

} // namespace Renderer_System
