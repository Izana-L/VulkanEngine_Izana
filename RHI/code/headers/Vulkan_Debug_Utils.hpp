#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Instance.hpp>
#include <Vulkan_Device.hpp>

#include <cstdint>
#include <type_traits>

namespace Renderer_System
{

    // Vulkan_Debug_Utils: names Vulkan objects and labels regions of a
    // command buffer through VK_EXT_debug_utils, so captures (RenderDoc,
    // Nsight) and validation messages show "Cluster_Grid_Frame1" instead
    // of a raw handle, and the passes of a frame appear as named groups.
    //
    // VK_EXT_debug_utils is an instance extension that Vulkan_Instance
    // enables only together with validation. Without it every call is a
    // no-op, so callers never test for its presence.
    //
    // Holds function pointers only: nothing to destroy. Not copyable or
    // movable, like the other members of the Renderer it belongs to.
    class Vulkan_Debug_Utils
    {
    public:

        // Loads the entry points when the instance enabled the extension.
        // _device must be the device whose objects are named.
        Vulkan_Debug_Utils(const Vulkan_Instance& _instance, const Vulkan_Device& _device);

        Vulkan_Debug_Utils(const Vulkan_Debug_Utils&) = delete;
        Vulkan_Debug_Utils& operator=(const Vulkan_Debug_Utils&) = delete;
        Vulkan_Debug_Utils(Vulkan_Debug_Utils&&) = delete;
        Vulkan_Debug_Utils& operator=(Vulkan_Debug_Utils&&) = delete;

        // True when names and labels reach the driver and the tools.
        bool Is_enabled() const { return set_object_name != nullptr; }

        // Assigns _name to a Vulkan object. _handle is any Vulkan handle,
        // dispatchable (pointer) or not (pointer or 64-bit integer,
        // depending on the platform); _type must describe it. The name is
        // copied by the driver. A null handle is ignored.
        template <typename HANDLE>
        void Set_name(HANDLE _handle, VkObjectType _type, const char* _name) const
        {
            Set_name_raw(To_raw_handle(_handle), _type, _name);
        }

        // Opens a labeled region in _command_buffer, with an RGB color for
        // the tools that display one. Regions nest; every Begin_label is
        // closed by an End_label in the same command buffer.
        void Begin_label(VkCommandBuffer _command_buffer, const char* _name, float _red, float _green, float _blue) const;
        void End_label(VkCommandBuffer _command_buffer) const;

    private:

        template <typename HANDLE>
        static uint64_t To_raw_handle(HANDLE _handle)
        {
            if constexpr (std::is_pointer_v<HANDLE>)
                return reinterpret_cast<uint64_t>(_handle);
            else
                return static_cast<uint64_t>(_handle);
        }

        void Set_name_raw(uint64_t _handle, VkObjectType _type, const char* _name) const;

        VkDevice                         device_handle = VK_NULL_HANDLE;
        PFN_vkSetDebugUtilsObjectNameEXT set_object_name = nullptr;
        PFN_vkCmdBeginDebugUtilsLabelEXT cmd_begin_label = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT   cmd_end_label = nullptr;
    };

    // Debug_Label_Scope: a labeled region that ends with the scope, so an
    // early return or an exception cannot leave the region open while the
    // command buffer keeps being recorded.
    class Debug_Label_Scope
    {
    public:
        Debug_Label_Scope(const Vulkan_Debug_Utils& _debug_utils, VkCommandBuffer _command_buffer, const char* _name,
                          float _red, float _green, float _blue)
            : debug_utils(_debug_utils), command_buffer(_command_buffer)
        {
            debug_utils.Begin_label(command_buffer, _name, _red, _green, _blue);
        }

        ~Debug_Label_Scope() { debug_utils.End_label(command_buffer); }

        Debug_Label_Scope(const Debug_Label_Scope&) = delete;
        Debug_Label_Scope& operator=(const Debug_Label_Scope&) = delete;

    private:
        const Vulkan_Debug_Utils& debug_utils;
        VkCommandBuffer           command_buffer;
    };

} // namespace Renderer_System
