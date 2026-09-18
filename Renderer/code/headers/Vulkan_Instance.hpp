#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Vulkan_Utils.hpp>
#include <vector>
#include <string>

namespace Renderer_System 
{
    // Vulkan_instance: owns the VkInstance, the root object of Vulkan.
    // Also manages validation layers and the debug messenger, which report
    // Vulkan API misuse (invalid parameters, resource leaks, etc.) directly
    // to the console instead of failing silently or crashing later.
    //
    // This is the first Vulkan object created and the last one destroyed -
    // everything else in the Renderer depends on it, directly or indirectly.
    class Vulkan_Instance 
    {
        VkInstance instance;
        VkDebugUtilsMessengerEXT debug_messenger;
        bool validation_enabled;
        uint32_t api_version;

    public:
        // Creates the VkInstance.
        // _enable_validation should be true in Debug builds and false in
        // Release (validation layers add CPU overhead and require the
        // Vulkan SDK to be installed on the machine running the app).
        // _application_name / _engine_name are passed to the driver and
        // may be used by some drivers to apply known per-engine optimizations.
        explicit Vulkan_Instance(
            bool _enable_validation,
            const std::string& _application_name = "Vulkan Engine",
            const std::string& _engine_name = "No Engine Name Yet"
        );

        ~Vulkan_Instance();

        // Copying a Vulkan_instance would mean two C++ objects owning the
        // same VkInstance handle, leading to double-destruction when both
        // go out of scope. Only moving is allowed, same pattern as Window.
        Vulkan_Instance(const Vulkan_Instance&) = delete;
        Vulkan_Instance& operator=(const Vulkan_Instance&) = delete;

        Vulkan_Instance(Vulkan_Instance&& _other) noexcept;
        Vulkan_Instance& operator=(Vulkan_Instance&& _other) noexcept;

        // Raw handle, needed by almost every other Vulkan_* class
        // (Vulkan_device, Vulkan_surface...) to create themselves.
        VkInstance Get_handle() const;

        // Whether validation layers ended up active on this instance.
        // Note this can be false even if you requested true, if the layers
        // weren't available on the system (see Check_validation_layer_support).
        bool Is_validation_enabled() const;

        // The actual Vulkan API version this instance was created with.
        // May be lower than the version requested in the constructor if
        // the system's driver doesn't support it.
        uint32_t Get_api_version() const;

    private:
        // Destroys the debug messenger (if active) and the VkInstance itself,
        // in the correct order (messenger depends on instance, so it must
        // be destroyed first). Shared by the destructor and move assignment
        // to avoid duplicating this cleanup logic in two places.
        void Destroy();

        // Queries the highest Vulkan API version supported by the system's
        // loader/driver, and returns the version we should actually request:
        // either what we asked for, or a lower version if that's all the
        // system supports.
        uint32_t Determine_api_version(uint32_t _requested_version) const;

        // Returns the list of validation layers to request (just the
        // standard Khronos validation layer for now - see the .cpp for
        // why this single layer is enough on modern Vulkan SDKs).
        std::vector<const char*> Get_required_validation_layers() const;

        // Returns the list of instance extensions required: whatever GLFW
        // needs to create a surface on this OS, plus the debug utils
        // extension if validation is enabled.
        std::vector<const char*> Get_required_extensions() const;

        // Checks that all requested validation layers are actually
        // available on this system before trying to enable them. Without
        // this check, requesting a missing layer would make vkCreateInstance
        // fail outright instead of gracefully falling back.
        bool Check_validation_layer_support(const std::vector<const char*>& _layers) const;

        // Prints the final list of activated extensions and layers to the
        // console after a successful vkCreateInstance call. Purely for
        // debugging - helps confirm exactly what got activated without
        // having to guess or attach a debugger.
        void Log_activated_extensions_and_layers(const std::vector<const char*>& _extensions,const std::vector<const char*>& _layers ) const;
      

        // Creates the "real" debug messenger that stays active for the
        // entire lifetime of this Vulkan_instance (as opposed to the
        // temporary one set up via pNext during vkCreateInstance itself).
        void Setup_debug_messenger();

        // The callback Vulkan calls whenever the validation layer has
        // something to report. The signature (parameter types, calling
        // convention, return type) is fixed by the Vulkan API - it must
        // match exactly or the function pointer won't be accepted.
        static VKAPI_ATTR VkBool32 VKAPI_CALL Debug_callback(
            VkDebugUtilsMessageSeverityFlagBitsEXT _severity,
            VkDebugUtilsMessageTypeFlagsEXT _type,
            const VkDebugUtilsMessengerCallbackDataEXT* _callback_data,
            void* _user_data
        );

        
    };

}