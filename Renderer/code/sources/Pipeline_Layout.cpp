#include <Pipeline_Layout.hpp>
#include <Vulkan_Utils.hpp>

#include <glm/glm.hpp>

#include <cassert>
#include <stdexcept>
#include <vector>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Pipeline_Layout::Pipeline_Layout(const Vulkan_Device& _device,
        VkDescriptorSetLayout _bindless_set_layout)
        : device_handle(_device.Get_logical_device_handle()),
        descriptor_set_layout(VK_NULL_HANDLE),
        pipeline_layout(VK_NULL_HANDLE)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Pipeline_Layout");

        Create_descriptor_set_layout();
        Create_pipeline_layout(_bindless_set_layout);
    }

    // ---------- Destructor ----------
    Pipeline_Layout::~Pipeline_Layout()
    {
        if (pipeline_layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device_handle, pipeline_layout, nullptr);
            pipeline_layout = VK_NULL_HANDLE;
        }
        if (descriptor_set_layout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device_handle, descriptor_set_layout, nullptr);
            descriptor_set_layout = VK_NULL_HANDLE;
        }
    }

    // ---------- Create_descriptor_set_layout ----------
    // Moved verbatim from Vulkan_Pipeline.
    void Pipeline_Layout::Create_descriptor_set_layout()
    {
        VkDescriptorSetLayoutBinding mvp_layout_binding{};
        mvp_layout_binding.binding = 0;
        mvp_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mvp_layout_binding.descriptorCount = 1;
        mvp_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        mvp_layout_binding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &mvp_layout_binding;

        VkResult result = vkCreateDescriptorSetLayout(
            device_handle, &layout_info, nullptr, &descriptor_set_layout);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create descriptor set layout: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }
    }

    // ---------- Create_pipeline_layout ----------
    // Moved verbatim from Vulkan_Pipeline.
    void Pipeline_Layout::Create_pipeline_layout(VkDescriptorSetLayout _bindless_set_layout)
    {
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(glm::mat4);   // 64 bytes

        std::vector<VkDescriptorSetLayout> set_layouts;
        set_layouts.push_back(descriptor_set_layout);

        if (_bindless_set_layout != VK_NULL_HANDLE)
            set_layouts.push_back(_bindless_set_layout);

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
        layout_info.pSetLayouts = set_layouts.data();
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;

        VkResult result = vkCreatePipelineLayout(
            device_handle, &layout_info, nullptr, &pipeline_layout);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create pipeline layout: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }
    }

} // namespace Renderer_System