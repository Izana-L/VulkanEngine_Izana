#include <Vulkan_Debug_Utils.hpp>

#include <iostream>

namespace Renderer_System
{

    Vulkan_Debug_Utils::Vulkan_Debug_Utils(const Vulkan_Instance& _instance, const Vulkan_Device& _device)
        : device_handle(_device.Get_logical_device_handle())
    {
        if (!_instance.Is_extension_enabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            std::cout << "[Vulkan_Debug_Utils] " << VK_EXT_DEBUG_UTILS_EXTENSION_NAME
                      << " not enabled: object names and command labels are disabled.\n";
            return;
        }

        // Commands of an instance extension, the device-level ones
        // included, are resolved through vkGetInstanceProcAddr.
        VkInstance instance = _instance.Get_handle();

        set_object_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT"));
        cmd_begin_label = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT"));
        cmd_end_label = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT"));

        // All or nothing: a Begin_label without its End_label would leave
        // the region open.
        if (set_object_name == nullptr || cmd_begin_label == nullptr || cmd_end_label == nullptr)
        {
            set_object_name = nullptr;
            cmd_begin_label = nullptr;
            cmd_end_label = nullptr;

            std::cerr << "[Vulkan_Debug_Utils] " << VK_EXT_DEBUG_UTILS_EXTENSION_NAME
                      << " is enabled but its entry points were not found: names and labels are disabled.\n";
            return;
        }

        std::cout << "[Vulkan_Debug_Utils] Object names and command labels enabled.\n";
    }

    void Vulkan_Debug_Utils::Set_name_raw(uint64_t _handle, VkObjectType _type, const char* _name) const
    {
        if (set_object_name == nullptr || _handle == 0 || _name == nullptr)
            return;

        VkDebugUtilsObjectNameInfoEXT name_info{};
        name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        name_info.objectType = _type;
        name_info.objectHandle = _handle;
        name_info.pObjectName = _name;

        // A failure only loses a name; it is not a reason to stop.
        const VkResult result = set_object_name(device_handle, &name_info);

        if (result != VK_SUCCESS)
            std::cerr << "[Vulkan_Debug_Utils] vkSetDebugUtilsObjectNameEXT failed for '" << _name << "'.\n";
    }

    void Vulkan_Debug_Utils::Begin_label(VkCommandBuffer _command_buffer, const char* _name, float _red, float _green, float _blue) const
    {
        if (cmd_begin_label == nullptr)
            return;

        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = _name;
        label.color[0] = _red;
        label.color[1] = _green;
        label.color[2] = _blue;
        label.color[3] = 1.0f;

        cmd_begin_label(_command_buffer, &label);
    }

    void Vulkan_Debug_Utils::End_label(VkCommandBuffer _command_buffer) const
    {
        if (cmd_end_label == nullptr)
            return;

        cmd_end_label(_command_buffer);
    }

} // namespace Renderer_System
