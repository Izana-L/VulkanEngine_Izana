#include <Pipeline_Layout.hpp>
#include <Vulkan_Utils.hpp>

#include <glm/glm.hpp>

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
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(glm::mat4);   // 64 bytes

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        // Siempre 4. Sin condicionales: si un set esta vacio, su layout vacio
        // ocupa la ranura igual. Asi el numero de un set nunca depende de si
        // otro existe.
        layout_info.setLayoutCount = Descriptor_Set::Count;
        layout_info.pSetLayouts = _layouts.Data();
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;

        VkResult result = vkCreatePipelineLayout(
            device_handle, &layout_info, nullptr, &pipeline_layout);

        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create pipeline layout: " +
                Vulkan_Utils::Vk_result_to_string(result));
        }
    }

} // namespace Renderer_System