#include <Vulkan_Instance.hpp>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <cassert>

namespace Renderer 
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
        bool _enable_validation,
        const std::string& _application_name,
        const std::string& _engine_name)

        // Initialize handles to VK_NULL_HANDLE first - if anything below
        // throws partway through, the destructor will see these as "never
        // created" and correctly skip trying to destroy them.
        : instance(VK_NULL_HANDLE),
        debug_messenger(VK_NULL_HANDLE),
        validation_enabled(_enable_validation),
        api_version(0) {

        std::vector<const char*> required_layers = Get_required_validation_layers();

        // Gracefully degrade instead of crashing: if validation was
        // requested but isn't available (e.g. Vulkan SDK not installed),
        // warn and continue without it rather than failing outright.
        if (validation_enabled && !Check_validation_layer_support(required_layers)) {
            std::cerr << "[Vulkan_instance] Validation layers requested but not available - disabling.\n";
            validation_enabled = false;
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

        std::vector<const char*> extensions = Get_required_extensions();

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
            create_info.pNext = &debug_create_info;
        }
        else {
            create_info.enabledLayerCount = 0;
            create_info.pNext = nullptr;
        }

        // The actual call that creates the VkInstance. Almost every Vulkan
        // function returns a VkResult; VK_SUCCESS means it worked, anything
        // else is a specific error code.
        VkResult result = vkCreateInstance(&create_info, nullptr, &instance);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create Vulkan instance: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        Log_activated_extensions_and_layers(extensions, validation_enabled ? required_layers : std::vector<const char*>{});

        // Only now create the "permanent" debug messenger, active for the
        // rest of this Vulkan_instance's lifetime.
        if (validation_enabled) {
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
            }
            vkDestroyInstance(instance, nullptr);
        }
    }

    // ---------- Move constructor ----------
    Vulkan_Instance::Vulkan_Instance(Vulkan_Instance&& _other) noexcept
        : instance(_other.instance),
        debug_messenger(_other.debug_messenger),
        validation_enabled(_other.validation_enabled),
        api_version(_other.api_version) {

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
            api_version = _other.api_version;

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
        assert(instance != VK_NULL_HANDLE && "Is_validation_enabled() called on a moved-from Vulkan_Instance");
        return validation_enabled;
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
            enumerate_version_function(&supported_version);
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

    // ---------- Get_required_extensions ----------
    std::vector<const char*> Vulkan_Instance::Get_required_extensions() const {
        uint32_t glfw_extension_count = 0;

        // GLFW knows which Vulkan instance extensions the current OS needs
        // to create a window surface (this differs between Windows, Linux, etc.)
        const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

        std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_extension_count);

        // VK_EXT_debug_utils is required to use the debug messenger system
        // (vkCreateDebugUtilsMessengerEXT etc.) - only needed if validation is on.
        if (validation_enabled)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            extensions.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }

        return extensions;
    }

    // ---------- Check_validation_layer_support ----------
    bool Vulkan_Instance::Check_validation_layer_support(const std::vector<const char*>& _layers) const {
        uint32_t layer_count = 0;

        // First call: ask Vulkan how many layers are available, without
        // requesting the actual data yet (standard "query size, then query
        // data" pattern used throughout the Vulkan API).
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

        std::vector<VkLayerProperties> available_layers(layer_count);

        // Second call: now actually fill the vector with the real data
        vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());

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
            std::cerr << "[Vulkan_instance] Failed to set up debug messenger: " << Vulkan_Utils::Vk_result_to_string(result) << "\n";
        }
    }

    // ---------- Debug_callback ----------
    VKAPI_ATTR VkBool32 VKAPI_CALL Vulkan_Instance::Debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT _severity,
        VkDebugUtilsMessageTypeFlagsEXT _type,
        const VkDebugUtilsMessengerCallbackDataEXT* _callback_data,
        void* _user_data) {

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