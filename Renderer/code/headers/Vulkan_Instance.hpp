#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Vulkan_Utils.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <string>

namespace Renderer_System 
{
    // Validation level requested at instance creation.
    //   Off          - no layers. The only option on machines without the
    //                  Vulkan SDK.
    //   Standard     - VK_LAYER_KHRONOS_validation: every API call is
    //                  checked on the CPU.
    //   Gpu_Assisted - Standard plus GPU-assisted validation (GPU-AV): the
    //                  layer instruments the shaders and checks on the GPU
    //                  what the CPU cannot see, such as out-of-range indices
    //                  into descriptor arrays or reads of descriptors that
    //                  were never written. Much slower; opt-in only.
    // A level the system cannot provide degrades to the previous one with
    // a warning; it never makes construction fail.
    enum class Validation_Mode : uint8_t
    {
        Off,
        Standard,
        Gpu_Assisted
    };
    // Vulkan_instance: owns the VkInstance, the root object of Vulkan.
    // Also manages validation layers and the debug messenger, which report
    // Vulkan API misuse (invalid parameters, resource leaks, etc.) directly
    // to the console instead of failing silently or crashing later.
    //
    // This is the first Vulkan object created and the last one destroyed -
    // everything else in the Renderer depends on it, directly or indirectly.
    //
    // Instance extensions are negotiated against what the loader reports,
    // never assumed:
    //   - the surface extensions GLFW needs are mandatory;
    //   - VK_EXT_debug_utils is enabled when validation is on and the
    //     extension exists;
    //   - with Validation_Mode::Gpu_Assisted, the extensions that carry
    //     the GPU-AV settings are taken from the validation layer itself
    //     (they are not listed by the loader): VK_EXT_layer_settings and
    //     VK_EXT_validation_features (deprecated), each one enabled and
    //     chained when the layer exposes it, so GPU-AV is requested
    //     through whichever of the two the installed layer honours;
    //   - VK_KHR_get_surface_capabilities2 + VK_KHR_surface_maintenance1
    //     (the instance half of VK_KHR_swapchain_maintenance1, whose
    //     device half Vulkan_Device enables) are enabled whenever the
    //     loader exposes them, independently of validation. Vulkan_Device
    //     reads Is_surface_maintenance1_enabled() and only enables the
    //     device extension when this half is present.
    //
    // GPU-assisted validation is verified, not assumed (see
    // Is_gpu_assisted_validation_enabled):
    //   - before the instance exists, a temporary instance without layers
    //     reads the real maxBoundDescriptorSets of every GPU. GPU-AV
    //     reserves one descriptor set slot, and the layer then reports one
    //     slot less; if a GPU has exactly the Descriptor_Set::Count slots
    //     the engine binds, GPU-AV would leave it unusable, so the mode
    //     degrades to standard validation instead;
    //   - once the instance exists, the limits reported through the layer
    //     are compared with the real ones: a reduced value proves the
    //     layer applied the GPU-AV settings; an unchanged one proves it
    //     ignored them, and the mode degrades to standard validation;
    //   - a GPU-AV setup problem that the layer reports later (a GPU
    //     without the features GPU-AV needs, detected at device creation)
    //     turns Is_gpu_assisted_validation_enabled() false.
    class Vulkan_Instance 
    {
        // State written by Debug_callback, reached through pUserData.
        // Heap-allocated so its address survives moves of the owning
        // Vulkan_Instance, which the messenger cannot follow.
        struct Debug_Report_State
        {
            // Set when the validation layer reports a message whose
            // identifier names GPU-assisted validation (setup problems,
            // GPU-AV disabling itself). The layer offers no query for its
            // own state; these messages are the only signal.
            std::atomic<bool> gpu_av_problem_reported{ false };
        };

        VkInstance instance;
        VkDebugUtilsMessengerEXT debug_messenger;
        bool validation_enabled;
        bool surface_maintenance1_enabled;
        // True only when validation is on, the validation layer exposes an
        // extension able to carry the GPU-AV settings, no GPU would be left
        // without a descriptor set slot, and the layer was not seen
        // ignoring the settings.
        bool gpu_assisted_enabled;
        uint32_t api_version;

        // Names of the instance extensions actually enabled, kept for
        // logging and for Vulkan_Device's queries.
        std::vector<std::string> enabled_extensions;

        // Never null except in a moved-from object.
        std::unique_ptr<Debug_Report_State> debug_state;

    public:
        // Creates the VkInstance.
        // _validation_mode should be Standard in Debug builds and Off in
        // Release (validation layers add CPU overhead and require the
        // Vulkan SDK to be installed on the machine running the app);
        // EngineCore selects it that way (Engine.cpp).
        // Gpu_Assisted is reserved for targeted debugging sessions: it
        // slows every draw noticeably (see Validation_Mode).
        // _application_name / _engine_name are passed to the driver and
        // may be used by some drivers to apply known per-engine optimizations.
        explicit Vulkan_Instance(Validation_Mode _validation_mode,const std::string& _application_name = "Vulkan Engine", const std::string& _engine_name = "No Engine Name Yet");

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
        // Can be false even when Standard or Gpu_Assisted was requested, if
        // the layers weren't available on the system (see
        // Check_validation_layer_support).
        bool Is_validation_enabled() const;

        // Whether GPU-assisted validation is active, as far as the engine
        // can tell. Always false when Is_validation_enabled() is false.
        // Also false when Gpu_Assisted was requested but:
        //   - the validation layer exposes neither VK_EXT_layer_settings
        //     nor VK_EXT_validation_features;
        //   - a GPU has exactly Descriptor_Set::Count descriptor set slots,
        //     so the slot GPU-AV reserves for itself would make that GPU
        //     unsuitable, or the real limits could not be read to rule it
        //     out;
        //   - the layer did not reduce maxBoundDescriptorSets, i.e. it
        //     ignored the GPU-AV settings;
        //   - the layer has reported a GPU-AV setup problem since (for
        //     example at device creation, when the GPU lacks a feature
        //     GPU-AV needs). The value can therefore change from true to
        //     false after vkCreateDevice.
        // When GPU-AV is active, the maxBoundDescriptorSets every query
        // returns already excludes the reserved slot.
        bool Is_gpu_assisted_validation_enabled() const;

        // Whether VK_KHR_get_surface_capabilities2 and
        // VK_KHR_surface_maintenance1 were enabled. This is the
        // precondition for VK_KHR_swapchain_maintenance1 on the device.
        bool Is_surface_maintenance1_enabled() const;

        // True if the named extension was enabled on this instance.
        bool Is_extension_enabled(const char* _name) const;

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

        // Builds the list of instance extensions to enable: whatever GLFW
        // needs to create a surface on this OS (mandatory), plus every
        // optional extension the loader actually reports. Records the
        // outcome in enabled_extensions / surface_maintenance1_enabled.
        std::vector<const char*> Select_extensions();

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
        // _user_data is the Debug_Report_State of the owning instance.
        static VKAPI_ATTR VkBool32 VKAPI_CALL Debug_callback( VkDebugUtilsMessageSeverityFlagBitsEXT _severity,
                                                              VkDebugUtilsMessageTypeFlagsEXT _type,
                                                              const VkDebugUtilsMessengerCallbackDataEXT* _callback_data,
                                                              void* _user_data);

        
    };

}
