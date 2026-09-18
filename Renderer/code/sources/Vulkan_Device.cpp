#include <Vulkan_Device.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <set>
#include <cstring>
#include <cassert>

namespace Renderer_System {

    namespace 
    {
        const std::vector<const char*> required_device_extensions = 
        {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };
    }

    // ---------- Constructor ----------
    Vulkan_Device::Vulkan_Device(const Vulkan_Instance& _instance, const Vulkan_Surface& _surface)
                                : physical_device(VK_NULL_HANDLE),
                                  logical_device(VK_NULL_HANDLE),
                                  graphics_queue(VK_NULL_HANDLE),
                                  present_queue(VK_NULL_HANDLE),
                                  swapchain_maintenance1_enabled(false),
                                  device_name(),
                                  bindless_supported(false)
    {
        VkInstance instance_handle = _instance.Get_handle();
        VkSurfaceKHR surface_handle = _surface.Get_handle();

        std::vector<VkPhysicalDevice> available_devices = Enumerate_physical_devices(instance_handle);
            
        if (available_devices.empty())
            throw std::runtime_error("No GPUs with Vulkan support found on this system");

        uint32_t best_score = 0;
        VkPhysicalDevice best_device = VK_NULL_HANDLE;

        for (VkPhysicalDevice candidate : available_devices) {
            uint32_t score = Rate_device_suitability(candidate, surface_handle);
            if (score > best_score) {
                best_score = score;
                best_device = candidate;
            }
        }

        if (best_device == VK_NULL_HANDLE)
            throw std::runtime_error("No suitable GPU found (missing required extensions or queue families)");
                
        physical_device = best_device;
        queue_family_indices = Find_queue_families(physical_device, surface_handle);

        Log_selected_device(physical_device);

        // ── Queue create infos ─────────────────────────────────────
        std::set<uint32_t> unique_queue_families = {
            queue_family_indices.graphics_family.value(),
            queue_family_indices.present_family.value()
        };

        std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
        float queue_priority = 1.0f;

        for (uint32_t family_index : unique_queue_families) {
            VkDeviceQueueCreateInfo queue_create_info{};
            queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_create_info.queueFamilyIndex = family_index;
            queue_create_info.queueCount = 1;
            queue_create_info.pQueuePriorities = &queue_priority;
            queue_create_infos.push_back(queue_create_info);
        }

        VkPhysicalDeviceFeatures device_features{};

        // ── Extensions ────────────────────────────────────────────
        std::vector<const char*> device_extensions = Get_required_device_extensions();

        bool swapchain_maintenance1_supported = Is_swapchain_maintenance1_supported();
        swapchain_maintenance1_enabled = swapchain_maintenance1_supported;

        if (swapchain_maintenance1_supported) {
            device_extensions.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            std::cout << "[Vulkan_Device] VK_KHR_swapchain_maintenance1 enabled.\n";
        }
        else {
            std::cout << "[Vulkan_Device] VK_KHR_swapchain_maintenance1 not available.\n";
        }

        // ── Bindless support check ─────────────────────────────────
        // Must be called BEFORE the if(bindless_supported) block below.
        bindless_supported = Check_bindless_support(physical_device);

        // ── Feature structs ───────────────────────────────────────
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_maintenance1_features{};
        swapchain_maintenance1_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
            
        swapchain_maintenance1_features.swapchainMaintenance1 = VK_TRUE;

        VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing_features{};
        descriptor_indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            

        if (bindless_supported)
        {
            // Array size known only at runtime.
            descriptor_indexing_features.runtimeDescriptorArray = VK_TRUE;

            // Allow unbound slots in the array (not every texture slot
            // needs to be filled as long as the shader never accesses it).
            descriptor_indexing_features.descriptorBindingPartiallyBound = VK_TRUE;

            // Allow non-uniform indexing in shaders (each invocation can
            // use a different texture index).
            descriptor_indexing_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

            // Allow updating the descriptor set while it is bound, so new
            // textures can be streamed in without stalling the pipeline.
            descriptor_indexing_features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

            std::cout << "[Vulkan_Device] Bindless descriptor indexing enabled.\n";
        }
        else
        {
            std::cout << "[Vulkan_Device] Bindless descriptor indexing NOT available "
                "(GPU or driver does not support required features).\n";
        }

        // ── Device create info ────────────────────────────────────
        VkDeviceCreateInfo device_create_info{};
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
        device_create_info.pQueueCreateInfos = queue_create_infos.data();
        device_create_info.pEnabledFeatures = &device_features;
        device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_create_info.ppEnabledExtensionNames = device_extensions.data();

        // ── pNext chain ───────────────────────────────────────────
        // Both feature structs use pNext — form a linked list so both
        // are active simultaneously. Order: descriptor_indexing →
        // swapchain_maintenance1 → null.
        if (bindless_supported && swapchain_maintenance1_enabled)
        {
            descriptor_indexing_features.pNext = &swapchain_maintenance1_features;
            device_create_info.pNext = &descriptor_indexing_features;
        }
        else if (bindless_supported)
        {
            device_create_info.pNext = &descriptor_indexing_features;
        }
        else if (swapchain_maintenance1_enabled)
        {
            device_create_info.pNext = &swapchain_maintenance1_features;
        }

        VkResult result = vkCreateDevice(
            physical_device, &device_create_info, nullptr, &logical_device);

        if (result != VK_SUCCESS)
            throw std::runtime_error(
                "Failed to create logical device: " +
                Vulkan_Utils::Vk_result_to_string(result));

        vkGetDeviceQueue(logical_device,
            queue_family_indices.graphics_family.value(), 0, &graphics_queue);
        vkGetDeviceQueue(logical_device,
            queue_family_indices.present_family.value(), 0, &present_queue);

        std::cout << "[Vulkan_Device] Logical device and queues created successfully.\n";
    }

    // ---------- Destructor ----------
    Vulkan_Device::~Vulkan_Device() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Device::Destroy() {
        if (logical_device != VK_NULL_HANDLE)
            vkDestroyDevice(logical_device, nullptr);
    }

    // ---------- Move constructor ----------
    Vulkan_Device::Vulkan_Device(Vulkan_Device&& _other) noexcept
        : physical_device(_other.physical_device),
        logical_device(_other.logical_device),
        graphics_queue(_other.graphics_queue),
        present_queue(_other.present_queue),
        queue_family_indices(_other.queue_family_indices),
        device_name(std::move(_other.device_name)),
        swapchain_maintenance1_enabled(_other.swapchain_maintenance1_enabled),
        bindless_supported(_other.bindless_supported)
    {
        _other.physical_device = VK_NULL_HANDLE;
        _other.logical_device = VK_NULL_HANDLE;
        _other.graphics_queue = VK_NULL_HANDLE;
        _other.present_queue = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Device& Vulkan_Device::operator=(Vulkan_Device&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            physical_device = _other.physical_device;
            logical_device = _other.logical_device;
            graphics_queue = _other.graphics_queue;
            present_queue = _other.present_queue;
            queue_family_indices = _other.queue_family_indices;
            device_name = std::move(_other.device_name);
            swapchain_maintenance1_enabled = _other.swapchain_maintenance1_enabled;
            bindless_supported = _other.bindless_supported;

            _other.physical_device = VK_NULL_HANDLE;
            _other.logical_device = VK_NULL_HANDLE;
            _other.graphics_queue = VK_NULL_HANDLE;
            _other.present_queue = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Getters ----------
    VkPhysicalDevice Vulkan_Device::Get_physical_device_handle() const {
        assert(physical_device != VK_NULL_HANDLE);
        return physical_device;
    }

    VkDevice Vulkan_Device::Get_logical_device_handle() const {
        assert(logical_device != VK_NULL_HANDLE);
        return logical_device;
    }

    VkQueue Vulkan_Device::Get_graphics_queue() const {
        assert(graphics_queue != VK_NULL_HANDLE);
        return graphics_queue;
    }

    VkQueue Vulkan_Device::Get_present_queue() const {
        assert(present_queue != VK_NULL_HANDLE);
        return present_queue;
    }

    const Queue_Family_Indices& Vulkan_Device::Get_queue_family_indices() const {
        assert(logical_device != VK_NULL_HANDLE);
        return queue_family_indices;
    }

    const std::string& Vulkan_Device::Get_device_name() const {
        assert(logical_device != VK_NULL_HANDLE);
        return device_name;
    }

    bool Vulkan_Device::Is_swapchain_maintenance1_enabled() const {
        return swapchain_maintenance1_enabled;
    }

    bool Vulkan_Device::Is_bindless_supported() const {
        return bindless_supported;
    }

    // ---------- Enumerate_physical_devices ----------
    std::vector<VkPhysicalDevice> Vulkan_Device::Enumerate_physical_devices(
        VkInstance _instance) const
    {
        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(_instance, &device_count, nullptr);

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(_instance, &device_count, devices.data());
        return devices;
    }

    // ---------- Find_supported_format ----------
    VkFormat Vulkan_Device::Find_supported_format(
        const std::vector<VkFormat>& _candidates,
        VkImageTiling                _tiling,
        VkFormatFeatureFlags         _features) const
    {
        assert(physical_device != VK_NULL_HANDLE);
        assert(!_candidates.empty());

        for (VkFormat format : _candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);

            if (_tiling == VK_IMAGE_TILING_LINEAR &&
                (properties.linearTilingFeatures & _features) == _features)
                return format;

            if (_tiling == VK_IMAGE_TILING_OPTIMAL &&
                (properties.optimalTilingFeatures & _features) == _features)
                return format;
        }

        throw std::runtime_error("Failed to find a supported format among the given candidates");
    }

    // ---------- Is_swapchain_maintenance1_supported ----------
    bool Vulkan_Device::Is_swapchain_maintenance1_supported() const
    {
        assert(physical_device != VK_NULL_HANDLE);

        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &extension_count, nullptr);

        std::vector<VkExtensionProperties> available_extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &extension_count, available_extensions.data());

        for (const auto& ext : available_extensions)
            if (std::strcmp(ext.extensionName,
                VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0)
                return true;

        return false;
    }

    // ---------- Check_bindless_support ----------
    bool Vulkan_Device::Check_bindless_support(VkPhysicalDevice _device) const
    {
        assert(_device != VK_NULL_HANDLE);

        VkPhysicalDeviceDescriptorIndexingFeatures indexing_features{};
        indexing_features.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &indexing_features;

        vkGetPhysicalDeviceFeatures2(_device, &features2);

        return indexing_features.runtimeDescriptorArray == VK_TRUE
            && indexing_features.descriptorBindingPartiallyBound == VK_TRUE
            && indexing_features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE
            && indexing_features.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
    }

    // ---------- Find_supported_depth_format ----------
    VkFormat Vulkan_Device::Find_supported_depth_format() const {
        assert(physical_device != VK_NULL_HANDLE);
        return Find_supported_format(
            { VK_FORMAT_D32_SFLOAT,
              VK_FORMAT_D32_SFLOAT_S8_UINT,
              VK_FORMAT_D24_UNORM_S8_UINT },
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    // ---------- Find_memory_type ----------
    uint32_t Vulkan_Device::Find_memory_type(
        uint32_t              _type_filter,
        VkMemoryPropertyFlags _properties) const
    {
        assert(physical_device != VK_NULL_HANDLE);

        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            bool type_is_allowed =
                (_type_filter & (1 << i)) != 0;
            bool has_required_properties =
                (memory_properties.memoryTypes[i].propertyFlags & _properties) == _properties;

            if (type_is_allowed && has_required_properties)
                return i;
        }

        throw std::runtime_error(
            "Failed to find a suitable GPU memory type for the given requirements");
    }

    // ---------- Is_device_suitable ----------
    bool Vulkan_Device::Is_device_suitable(
        VkPhysicalDevice _device, VkSurfaceKHR _surface) const
    {
        assert(_device != VK_NULL_HANDLE);
        assert(_surface != VK_NULL_HANDLE);

        // Extended dynamic state (vkCmdSetCullMode, vkCmdSetDepthTestEnable, …)
        // is core AND required in Vulkan 1.3 — no feature bit, no extension.
        // Vulkan_Instance::Determine_api_version caps the *instance* version;
        // this is the *device* version, which is what actually gates those
        // entry points.
        VkPhysicalDeviceProperties device_properties{};
        vkGetPhysicalDeviceProperties(_device, &device_properties);
        bool api_1_3_supported = device_properties.apiVersion >= VK_API_VERSION_1_3;

        Queue_Family_Indices indices = Find_queue_families(_device, _surface);
        bool extensions_supported = Check_device_extension_support(_device);

        bool swapchain_adequate = false;
        if (extensions_supported) {
            uint32_t format_count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                _device, _surface, &format_count, nullptr);

            uint32_t present_mode_count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                _device, _surface, &present_mode_count, nullptr);

            swapchain_adequate = (format_count > 0) && (present_mode_count > 0);
        }

        return api_1_3_supported && indices.Is_complete() && extensions_supported && swapchain_adequate;
      
    }

    // ---------- Check_device_extension_support ----------
    bool Vulkan_Device::Check_device_extension_support(VkPhysicalDevice _device) const
    {
        assert(_device != VK_NULL_HANDLE);

        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(
            _device, nullptr, &extension_count, nullptr);

        std::vector<VkExtensionProperties> available_extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(
            _device, nullptr, &extension_count, available_extensions.data());

        for (const char* required : required_device_extensions) {
            bool found = false;
            for (const auto& ext : available_extensions)
                if (std::strcmp(required, ext.extensionName) == 0) {
                    found = true;
                    break;
                }
            if (!found) return false;
        }

        return true;
    }

    // ---------- Find_queue_families ----------
    Queue_Family_Indices Vulkan_Device::Find_queue_families(
        VkPhysicalDevice _device, VkSurfaceKHR _surface) const
    {
        assert(_device != VK_NULL_HANDLE);
        assert(_surface != VK_NULL_HANDLE);

        Queue_Family_Indices indices;

        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            _device, &queue_family_count, nullptr);

        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(
            _device, &queue_family_count, queue_families.data());

        for (uint32_t i = 0; i < queue_family_count; ++i) {
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                indices.graphics_family = i;

            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(
                _device, i, _surface, &present_support);

            if (present_support == VK_TRUE)
                indices.present_family = i;

            if (indices.Is_complete()) break;
        }

        return indices;
    }

    // ---------- Rate_device_suitability ----------
    uint32_t Vulkan_Device::Rate_device_suitability(
        VkPhysicalDevice _device, VkSurfaceKHR _surface) const
    {
        assert(_device != VK_NULL_HANDLE);
        assert(_surface != VK_NULL_HANDLE);

        if (!Is_device_suitable(_device, _surface)) return 0;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device, &properties);

        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(_device, &memory_properties);

        uint32_t score = 0;

        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 1000;
        else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score += 100;

        score += properties.limits.maxImageDimension2D;

        for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i)
            if (memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                score += static_cast<uint32_t>(
                    memory_properties.memoryHeaps[i].size / (1024 * 1024));

        return score;
    }

    // ---------- Get_required_device_extensions ----------
    std::vector<const char*> Vulkan_Device::Get_required_device_extensions() const {
        return required_device_extensions;
    }

    // ---------- Log_selected_device ----------
    void Vulkan_Device::Log_selected_device(VkPhysicalDevice _device) const {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device, &properties);

        const_cast<Vulkan_Device*>(this)->device_name = properties.deviceName;

        std::cout << "[Vulkan_Device] Selected GPU: " << properties.deviceName << "\n";
        std::cout << "[Vulkan_Device] Driver version: " << properties.driverVersion << "\n";
        std::cout << "[Vulkan_Device] Vulkan API version: "
            << VK_API_VERSION_MAJOR(properties.apiVersion) << "."
            << VK_API_VERSION_MINOR(properties.apiVersion) << "."
            << VK_API_VERSION_PATCH(properties.apiVersion) << "\n";
    }

} // namespace Renderer