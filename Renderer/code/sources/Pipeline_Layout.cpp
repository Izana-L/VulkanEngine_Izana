#include <Pipeline_Layout.hpp>
#include <Vulkan_Utils.hpp>
#include <Frame_Data.hpp>

#include <cassert>
#include <stdexcept>
#include <vector>

namespace Renderer_System
{

    // One range covering the whole Push_Constants block for both stages:
// the vertex shader reads the model matrix, the fragment shader reads
// the base color and the texture index. The size is taken from the C++
// mirror so the two cannot drift apart.
    Pipeline_Layout::Pipeline_Layout(const Vulkan_Device& _device,
        const Descriptor_Layout_Cache& _layouts)
        : Pipeline_Layout(_device, _layouts,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            static_cast<uint32_t>(sizeof(Push_Constants)))
    {
        static_assert(sizeof(Push_Constants) <= 128,
            "Push_Constants exceeds the 128-byte minimum maxPushConstantsSize");
    }

    Pipeline_Layout::Pipeline_Layout(const Vulkan_Device& _device,
        const Descriptor_Layout_Cache& _layouts,
        VkShaderStageFlags _push_constant_stages,
        uint32_t _push_constant_size)
        : device_handle(_device.Get_logical_device_handle()),
        pipeline_layout(VK_NULL_HANDLE)
    {
        // Push constant range rules (vkCreatePipelineLayout valid usage):
        // size is a multiple of 4, stageFlags is non-zero, and offset + size
        // does not exceed maxPushConstantsSize. Checked here so a violation
        // reports this class instead of a validation layer message.
        if (_push_constant_size % 4 != 0)
            throw std::invalid_argument("Pipeline_Layout: push constant size must be a multiple of 4");

        if (_push_constant_size > 0 && _push_constant_stages == 0)
            throw std::invalid_argument("Pipeline_Layout: push constant range needs at least one shader stage");

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device.Get_physical_device_handle(), &properties);

        if (_push_constant_size > properties.limits.maxPushConstantsSize)
            throw std::invalid_argument("Pipeline_Layout: push constant size exceeds maxPushConstantsSize");

        VkPushConstantRange push_range{};
        push_range.stageFlags = _push_constant_stages;
        push_range.offset = 0;
        push_range.size = _push_constant_size;

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        // Always Descriptor_Set::Count. No conditionals: if a set is empty,
        // its empty layout still occupies the slot, so a set's number never
        // depends on whether another set exists.
        layout_info.setLayoutCount = Descriptor_Set::Count;
        layout_info.pSetLayouts = _layouts.Data();
        // A zero-sized range is invalid in Vulkan: no push constants means
        // no range at all.
        layout_info.pushConstantRangeCount = (_push_constant_size > 0) ? 1u : 0u;
        layout_info.pPushConstantRanges = (_push_constant_size > 0) ? &push_range : nullptr;

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
