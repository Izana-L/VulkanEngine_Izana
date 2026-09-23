#include <Vulkan_Instance.hpp>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <cassert>
#include <unordered_set>

namespace Renderer_System 
{

    namespace 
    {
        // The modern, unified Khronos validation layer. Older Vulkan
        // tutorials list several separate layers (VK_LAYER_LUNARG_*) -
        // those are deprecated; this single layer now covers everything
        // they used to do (parameter validation, object lifetime tracking,
        // basic synchronization checks, best-practice warnings, etc.).
        // Wrapped in an anonymous namespace so it's only visible in this .cpp.
        const std::vector<const char*> validation_layers = 
        {
            "VK_LAYER_KHRONOS_validation"
        };

        // Every instance extension exposed, by name.
         //   _layer_name == nullptr - the extensions of the loader, the
         //                            drivers and the implicit layers.
         //   _layer_name == a layer - only the extensions that explicit layer
         //                            provides itself, which the nullptr
         //                            query does not list. VK_EXT_layer_settings
         //                            and VK_EXT_validation_features are
         //                            obtained this way from
         //                            VK_LAYER_KHRONOS_validation.
         // The layer must be available (see Check_validation_layer_support);
         // otherwise the query fails with VK_ERROR_LAYER_NOT_PRESENT.
        std::unordered_set<std::string> Enumerate_instance_extensions(const char* _layer_name = nullptr)
        {
            uint32_t count = 0;
            VK_CHECK(vkEnumerateInstanceExtensionProperties(_layer_name, &count, nullptr),
                "Vulkan_Instance: enumerate instance extensions");

            std::vector<VkExtensionProperties> properties(count);
            VK_CHECK(vkEnumerateInstanceExtensionProperties(_layer_name, &count, properties.data()),
                "Vulkan_Instance: enumerate instance extensions");

            std::unordered_set<std::string> names;
            for (const VkExtensionProperties& extension : properties)
                names.insert(extension.extensionName);

            return names;
        }
    }

    // ---------- Manual extension function loading ----------
    // vkCreateDebugUtilsMessengerEXT / vkDestroyDebugUtilsMessengerEXT belong
    // to an EXTENSION (VK_EXT_debug_utils), not Vulkan core. Extension
    // functions aren't guaranteed to exist and can't be linked directly -
    // they must be looked up at runtime via vkGetInstanceProcAddr, which
    // returns nullptr if the current driver doesn't support them.

    static VkResult Create_debug_utils_messenger_ext(
        VkInstance _instance,
        const VkDebugUtilsMessengerCreateInfoEXT* _create_info,
        const VkAllocationCallbacks* _allocator,
        VkDebugUtilsMessengerEXT* _debug_messenger) {

        auto function = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT"));

        if (function != nullptr) {
            return function(_instance, _create_info, _allocator, _debug_messenger);
        }
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    static void Destroy_debug_utils_messenger_ext(
        VkInstance _instance,
        VkDebugUtilsMessengerEXT _debug_messenger,
        const VkAllocationCallbacks* _allocator) {

        auto function = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(_instance, "vkDestroyDebugUtilsMessengerEXT"));

        if (function != nullptr) {
            function(_instance, _debug_messenger, _allocator);
        }
        // If the function doesn't exist, there's nothing valid to destroy anyway.
    }

    // ---------- Constructor ----------
    Vulkan_Instance::Vulkan_Instance(
        Validation_Mode _validation_mode,
        const std::string& _application_name,
        const std::string& _engine_name)

        // Initialize handles to VK_NULL_HANDLE first - if anything below
        // throws partway through, the destructor will see these as "never
        // created" and correctly skip trying to destroy them.
        : instance(VK_NULL_HANDLE),
        debug_messenger(VK_NULL_HANDLE),
        validation_enabled(_validation_mode != Validation_Mode::Off),
        gpu_assisted_enabled(_validation_mode == Validation_Mode::Gpu_Assisted),
        surface_maintenance1_enabled(false),
        api_version(0) {

        std::vector<const char*> required_layers = Get_required_validation_layers();

        // Gracefully degrade instead of crashing: if validation was
        // requested but isn't available (e.g. Vulkan SDK not installed),
        // warn and continue without it rather than failing outright.
        if (validation_enabled && !Check_validation_layer_support(required_layers)) {
            std::cerr << "[Vulkan_instance] Validation layers requested but not available - disabling.\n";
            validation_enabled = false;
            // GPU-AV runs inside the validation layer: it degrades with it.
            gpu_assisted_enabled = false;
        }

        // Cap the requested API version to whatever this system's driver
        // actually supports, instead of blindly requesting 1.3 and letting
        // vkCreateInstance fail on older drivers.
        api_version = Determine_api_version(VK_API_VERSION_1_3);

        // VkApplicationInfo: descriptive metadata about this application.
        // Some drivers use the engine/app name+version to apply known,
        // engine-specific optimizations or workarounds (most relevant for
        // famous engines, but good practice to fill in correctly regardless).
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = _application_name.c_str();
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName = _engine_name.c_str();
        app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion = api_version;

        std::vector<const char*> extensions = Select_extensions();

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();

        // Temporary debug messenger config, attached via pNext below.
        // This captures validation messages that occur DURING vkCreateInstance
        // and vkDestroyInstance themselves - a window of time where the
        // "real" messenger (created via Setup_debug_messenger) doesn't exist yet.
        VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};
        // GPU-AV configuration, chained via pNext below when
        // gpu_assisted_enabled. Declared at this scope because pNext only
        // stores pointers: every struct in the chain must stay alive until
        // vkCreateInstance returns. Only one of the two is chained, the one
        // matching the extension Select_extensions enabled:
        //   - VK_EXT_layer_settings: layer setting "gpuav_enable". Setting
        //     names have changed between SDK releases; the reference is the
        //     khronos_validation documentation of the installed SDK.
        //   - VK_EXT_validation_features (deprecated): the equivalent flag,
        //     plus RESERVE_BINDING_SLOT, which makes the layer report
        //     maxBoundDescriptorSets minus one, so the slot GPU-AV takes
        //     for itself is never offered to the application.
        const VkBool32 gpu_av_enable = VK_TRUE;

        VkLayerSettingEXT gpu_av_setting{};
        gpu_av_setting.pLayerName = validation_layers.front();
        gpu_av_setting.pSettingName = "gpuav_enable";
        gpu_av_setting.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
        gpu_av_setting.valueCount = 1;
        gpu_av_setting.pValues = &gpu_av_enable;

        VkLayerSettingsCreateInfoEXT layer_settings_info{};
        layer_settings_info.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
        layer_settings_info.settingCount = 1;
        layer_settings_info.pSettings = &gpu_av_setting;

        const VkValidationFeatureEnableEXT gpu_av_features[] =
        {
            VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
            VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT
        };

        VkValidationFeaturesEXT validation_features{};
        validation_features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validation_features.enabledValidationFeatureCount = static_cast<uint32_t>(std::size(gpu_av_features));
        validation_features.pEnabledValidationFeatures = gpu_av_features;
        if (validation_enabled) {
            create_info.enabledLayerCount = static_cast<uint32_t>(required_layers.size());
            create_info.ppEnabledLayerNames = required_layers.data();

            debug_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debug_create_info.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug_create_info.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug_create_info.pfnUserCallback = Debug_callback;

            // pNext is Vulkan's mechanism for "extending" a struct with
            // additional data without changing its original definition.
            // Only chained when the debug utils extension was enabled.
            if (Is_extension_enabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                create_info.pNext = &debug_create_info;

            // GPU-AV settings are prepended to the chain: whatever was
            // already linked (the debug messenger info) stays linked behind
            // them instead of being replaced.
            if (gpu_assisted_enabled) 
            {
                if (Is_extension_enabled(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) 
                {
                    layer_settings_info.pNext = create_info.pNext;
                    create_info.pNext = &layer_settings_info;
                }
                else if (Is_extension_enabled(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) 
                {
                    validation_features.pNext = create_info.pNext;
                    create_info.pNext = &validation_features;
                }
            }
        }
        else 
        {
            create_info.enabledLayerCount = 0;
            create_info.pNext = nullptr;
        }

        // The actual call that creates the VkInstance. Almost every Vulkan
        // function returns a VkResult; VK_SUCCESS means it worked, anything
        // else is a specific error code.
        VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance), "Failed to create Vulkan instance");

        Log_activated_extensions_and_layers(extensions, validation_enabled ? required_layers : std::vector<const char*>{});

        if (gpu_assisted_enabled) 
        {
            std::cout << "[Vulkan_instance] GPU-assisted validation ENABLED (slow), configured through "
                      << (Is_extension_enabled(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME) ? VK_EXT_LAYER_SETTINGS_EXTENSION_NAME : VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)
                      << ".\n";
        }

        // Only now create the "permanent" debug messenger, active for the
        // rest of this Vulkan_instance's lifetime.
        if (validation_enabled && Is_extension_enabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            Setup_debug_messenger();
        }
    }

    // ---------- Destructor ----------
    Vulkan_Instance::~Vulkan_Instance() {
        Destroy();
    }

    // ---------- Destroy (shared cleanup logic) ----------
    void Vulkan_Instance::Destroy() {
        // Only attempt cleanup if instance was actually created - if the
        // constructor threw before vkCreateInstance succeeded, this stays
        // VK_NULL_HANDLE and there's nothing to destroy.
        if (instance != VK_NULL_HANDLE) {
            // Destroy in reverse order of creation: messenger depends on
            // instance, so it must go first.
            if (debug_messenger != VK_NULL_HANDLE) {
                Destroy_debug_utils_messenger_ext(instance, debug_messenger, nullptr);
                debug_messenger = VK_NULL_HANDLE;
            }
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }

    // ---------- Move constructor ----------
    Vulkan_Instance::Vulkan_Instance(Vulkan_Instance&& _other) noexcept
        : instance(_other.instance),
        debug_messenger(_other.debug_messenger),
        validation_enabled(_other.validation_enabled),
        gpu_assisted_enabled(_other.gpu_assisted_enabled),
        surface_maintenance1_enabled(_other.surface_maintenance1_enabled),
        api_version(_other.api_version),
        enabled_extensions(std::move(_other.enabled_extensions)) {

        // Leave the moved-from object in a valid empty state so its
        // destructor doesn't try to destroy handles "this" now owns.
        _other.instance = VK_NULL_HANDLE;
        _other.debug_messenger = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Instance& Vulkan_Instance::operator=(Vulkan_Instance&& _other) noexcept {
        if (this != &_other) {
            // Release whatever this object currently owns before taking
            // ownership of _other's resources, to avoid leaking them.
            Destroy();

            instance = _other.instance;
            debug_messenger = _other.debug_messenger;
            validation_enabled = _other.validation_enabled;
            gpu_assisted_enabled = _other.gpu_assisted_enabled;
            surface_maintenance1_enabled = _other.surface_maintenance1_enabled;
            api_version = _other.api_version;
            enabled_extensions = std::move(_other.enabled_extensions);

            _other.instance = VK_NULL_HANDLE;
            _other.debug_messenger = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Get_handle ----------
    VkInstance Vulkan_Instance::Get_handle() const 
    {
        assert(instance != VK_NULL_HANDLE && "Get_handle() called on a moved-from Vulkan_Instance");
        return instance;
    }

    // ---------- Is_validation_enabled ----------
    bool Vulkan_Instance::Is_validation_enabled() const 
    {
        return validation_enabled;
    }
    // ---------- Is_gpu_assisted_validation_enabled ----------
    bool Vulkan_Instance::Is_gpu_assisted_validation_enabled() const
    {
        return gpu_assisted_enabled;
    }
    bool Vulkan_Instance::Is_surface_maintenance1_enabled() const
    {
        return surface_maintenance1_enabled;
    }

    bool Vulkan_Instance::Is_extension_enabled(const char* _name) const
    {
        for (const std::string& name : enabled_extensions)
            if (name == _name) return true;

        return false;
    }

    // ---------- Get_api_version ----------
    uint32_t Vulkan_Instance::Get_api_version() const 
    {
        assert(instance != VK_NULL_HANDLE && "Get_api_version() called on a moved-from Vulkan_Instance");
        return api_version;
    }

    // ---------- Determine_api_version ----------
    uint32_t Vulkan_Instance::Determine_api_version(uint32_t _requested_version) const {
        uint32_t supported_version = VK_API_VERSION_1_0;

        // vkEnumerateInstanceVersion doesn't exist on very old systems
        // (pre-Vulkan-1.1 loaders) - look it up manually instead of
        // assuming it's always linkable, same pattern as the debug
        // messenger functions above.
        auto enumerate_version_function = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));

        if (enumerate_version_function != nullptr) {
            VK_CHECK(enumerate_version_function(&supported_version), "Vulkan_Instance: enumerate instance version");
        }
        // If the function isn't found, we silently assume Vulkan 1.0,
        // which is a safe, conservative fallback.

        std::cout << "[Vulkan_instance] System supports up to Vulkan "
            << VK_API_VERSION_MAJOR(supported_version) << "."
            << VK_API_VERSION_MINOR(supported_version) << "."
            << VK_API_VERSION_PATCH(supported_version) << "\n";

        if (supported_version < _requested_version) {
            std::cerr << "[Vulkan_instance] Requested Vulkan "
                << VK_API_VERSION_MAJOR(_requested_version) << "."
                << VK_API_VERSION_MINOR(_requested_version)
                << " is not supported, falling back to "
                << VK_API_VERSION_MAJOR(supported_version) << "."
                << VK_API_VERSION_MINOR(supported_version) << "\n";
            return supported_version;
        }

        return _requested_version;
    }

    // ---------- Get_required_validation_layers ----------
    std::vector<const char*> Vulkan_Instance::Get_required_validation_layers() const {
        return validation_layers;
    }

    // ---------- Select_extensions ----------
    std::vector<const char*> Vulkan_Instance::Select_extensions() {
        uint32_t glfw_extension_count = 0;

        // GLFW knows which Vulkan instance extensions the current OS needs
        // to create a window surface (this differs between Windows, Linux, etc.)
        const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

        if (glfw_extensions == nullptr || glfw_extension_count == 0) {
            throw std::runtime_error("Vulkan_Instance: GLFW could not provide the instance extensions "
                "required to create a surface (no Vulkan-capable loader found)");
        }

        const std::unordered_set<std::string> available = Enumerate_instance_extensions();

        std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_extension_count);

        for (const char* name : extensions) {
            if (available.count(name) == 0) {
                throw std::runtime_error(std::string("Vulkan_Instance: required instance extension ") +
                    name + " is not available");
            }
        }

        // Optional extensions: enabled only when the loader reports them,
        // so vkCreateInstance never fails over an extension this engine can
        // live without.
        const auto try_enable = [&](const char* name) -> bool
            {
                if (available.count(name) == 0) return false;
                extensions.push_back(name);
                return true;
            };

        if (validation_enabled && !try_enable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            std::cerr << "[Vulkan_instance] VK_EXT_debug_utils not available - validation messages "
                "will not be reported through the debug messenger.\n";
        }

        // GPU-AV is configured through an extension the validation layer
        // provides itself, so it is looked up in the layer's own list, not
        // in `available`. VK_EXT_layer_settings is preferred;
        // VK_EXT_validation_features is deprecated and only kept as a
        // fallback for SDKs that predate the former. Without either, the
        // mode degrades to standard validation.
        if (gpu_assisted_enabled) 
        {
            const std::unordered_set<std::string> layer_extensions = Enumerate_instance_extensions(validation_layers.front());

            if (layer_extensions.count(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME) != 0) 
            {
                extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
            }
            else if (layer_extensions.count(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) != 0) 
            {
                extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            }
            else 
            {
                std::cerr << "[Vulkan_instance] GPU-assisted validation requested but the validation layer exposes "
                    "neither VK_EXT_layer_settings nor VK_EXT_validation_features - falling back to standard validation.\n";
                gpu_assisted_enabled = false;
            }
        }

        // Instance half of swapchain maintenance1. It is independent of
        // validation: without it, Vulkan_Device must not enable
        // VK_KHR_swapchain_maintenance1 and the Renderer falls back to
        // per-image semaphores alone for present synchronization.
        // The KHR names are preferred; drivers that only ship the EXT
        // promotion predecessors are accepted too (same functionality).
        const bool capabilities2 = try_enable(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        const bool maintenance1 = capabilities2 &&
            (try_enable(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) ||
                try_enable(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME));

        surface_maintenance1_enabled = maintenance1;

        if (!surface_maintenance1_enabled) {
            std::cout << "[Vulkan_instance] Surface maintenance1 not available on this loader - "
                "present fences disabled.\n";
        }

        enabled_extensions.assign(extensions.begin(), extensions.end());

        return extensions;
    }

    // ---------- Check_validation_layer_support ----------
    bool Vulkan_Instance::Check_validation_layer_support(const std::vector<const char*>& _layers) const {
        uint32_t layer_count = 0;

        // First call: ask Vulkan how many layers are available, without
        // requesting the actual data yet (standard "query size, then query
        // data" pattern used throughout the Vulkan API).
        VK_CHECK(vkEnumerateInstanceLayerProperties(&layer_count, nullptr),
            "Vulkan_Instance: enumerate instance layers");

        std::vector<VkLayerProperties> available_layers(layer_count);

        // Second call: now actually fill the vector with the real data
        VK_CHECK(vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data()),
            "Vulkan_Instance: enumerate instance layers");

        for (const char* requested_layer : _layers) {
            bool found = false;

            for (const auto& available_layer : available_layers) {
                if (std::strcmp(requested_layer, available_layer.layerName) == 0) {
                    found = true;
                    break;
                }
            }

            // If even one requested layer is missing, treat the whole
            // check as failed - we want all-or-nothing here.
            if (!found) {
                return false;
            }
        }

        return true;
    }

    // ---------- Log_activated_extensions_and_layers ----------
    void Vulkan_Instance::Log_activated_extensions_and_layers(
        const std::vector<const char*>& _extensions,
        const std::vector<const char*>& _layers) const {

        std::cout << "[Vulkan_instance] Activated " << _extensions.size() << " extension(s):\n";
        for (const char* extension_name : _extensions) {
            std::cout << "    - " << extension_name << "\n";
        }

        if (!_layers.empty()) {
            std::cout << "[Vulkan_instance] Activated " << _layers.size() << " validation layer(s):\n";
            for (const char* layer_name : _layers) {
                std::cout << "    - " << layer_name << "\n";
            }
        }
        else {
            std::cout << "[Vulkan_instance] No validation layers activated.\n";
        }
    }


    // ---------- Setup_debug_messenger ----------
    void Vulkan_Instance::Setup_debug_messenger() {
        // Same configuration as the temporary one set up inside the
        // constructor, but this time creating the permanent messenger that
        // stays active for the lifetime of this Vulkan_instance.
        VkDebugUtilsMessengerCreateInfoEXT create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        create_info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        create_info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        create_info.pfnUserCallback = Debug_callback;

        VkResult result = Create_debug_utils_messenger_ext(instance, &create_info, nullptr, &debug_messenger);
        if (result != VK_SUCCESS) {
            debug_messenger = VK_NULL_HANDLE;
            std::cerr << "[Vulkan_instance] Failed to set up debug messenger: " << Vulkan_Utils::Vk_result_to_string(result) << "\n";
        }
    }

    // ---------- Debug_callback ----------
    VKAPI_ATTR VkBool32 VKAPI_CALL Vulkan_Instance::Debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT _severity,
        VkDebugUtilsMessageTypeFlagsEXT /*_type*/,
        const VkDebugUtilsMessengerCallbackDataEXT* _callback_data,
        void* /*_user_data*/) {

        // Only print warnings and errors - verbose/info messages are
        // extremely noisy (internal layer logging) and rarely useful day-to-day.
        if (_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            std::cerr << "[Vulkan Validation] " << _callback_data->pMessage << "\n";
        }

        // VK_FALSE means "don't abort the Vulkan call that triggered this
        // message" - the standard/expected return value. VK_TRUE is reserved
        // for specific conformance testing tools, not normal engine code.
        return VK_FALSE;
    }

}
