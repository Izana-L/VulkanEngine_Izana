#include <Vulkan_Device.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <set>
#include <cstring>
#include <cassert>

namespace Renderer {

    namespace {
        // Minimum device extensions required. VK_KHR_swapchain is essential -
        // without it, the GPU can't present rendered images to a window at all.
        const std::vector<const char*> required_device_extensions = {
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
        device_name()
    {

        VkInstance instance_handle = _instance.Get_handle();
        VkSurfaceKHR surface_handle = _surface.Get_handle();

        std::vector<VkPhysicalDevice> available_devices = Enumerate_physical_devices(instance_handle);

        if (available_devices.empty()) {
            throw std::runtime_error("No GPUs with Vulkan support found on this system");
        }

        // Score every candidate GPU and keep track of the best one.
        // Devices that fail Is_device_suitable get a score of 0 and are
        // effectively never picked (unless literally everything scores 0,
        // in which case we correctly fail below).
        uint32_t best_score = 0;
        VkPhysicalDevice best_device = VK_NULL_HANDLE;

        for (VkPhysicalDevice candidate : available_devices) {
            uint32_t score = Rate_device_suitability(candidate, surface_handle);

            if (score > best_score) {
                best_score = score;
                best_device = candidate;
            }
        }

        if (best_device == VK_NULL_HANDLE) {
            throw std::runtime_error("No suitable GPU found (missing required extensions or queue families)");
        }

        physical_device = best_device;
        queue_family_indices = Find_queue_families(physical_device, surface_handle);

        Log_selected_device(physical_device);

        // ---------- Logical device creation ----------

        // Build the set of unique queue family indices we need to create
        // queues for. std::set automatically deduplicates - if graphics
        // and present happen to be the same family index, we only create
        // one VkDeviceQueueCreateInfo for it, not two.
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

        // No special GPU features requested yet (anisotropic filtering,
        // geometry shaders, etc.) - left as default/false for now. We'll
        // enable specific features here later as the renderer needs them.
        VkPhysicalDeviceFeatures device_features{};

        std::vector<const char*> device_extensions = Get_required_device_extensions();
        bool swapchain_maintenance1_supported = Is_swapchain_maintenance1_supported();
        swapchain_maintenance1_enabled = swapchain_maintenance1_supported;
        if (swapchain_maintenance1_supported) {
            device_extensions.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            std::cout << "[Vulkan_Device] VK_KHR_swapchain_maintenance1 enabled.\n";
        }
        else
        {
            std::cout << "[Vulkan_Device] VK_KHR_swapchain_maintenance1 not available, falling back to FIFO.\n";
        }
        // Feature struct for VK_KHR_swapchain_maintenance1 - must be
        // chained into VkDeviceCreateInfo via pNext, in addition to
        // adding the extension name to ppEnabledExtensionNames. Without
        // this, the extension is "present" but its features aren't active,
        // causing vkQueuePresentKHR to reject VkSwapchainPresentFenceInfoEXT.
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_maintenance1_features{};
        swapchain_maintenance1_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        swapchain_maintenance1_features.swapchainMaintenance1 = VK_TRUE;

        VkDeviceCreateInfo device_create_info{};
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
        device_create_info.pQueueCreateInfos = queue_create_infos.data();
        device_create_info.pEnabledFeatures = &device_features;
        device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_create_info.ppEnabledExtensionNames = device_extensions.data();

        // Chain the maintenance1 feature only if the extension is available
        if (swapchain_maintenance1_enabled) {
            device_create_info.pNext = &swapchain_maintenance1_features;
        }
        // Device-level validation layers are deprecated in modern Vulkan
        // (instance-level validation, already set up in Vulkan_Instance,
        // covers everything) - intentionally not setting enabledLayerCount
        // here, it's ignored by current drivers anyway.

        VkResult result = vkCreateDevice(physical_device, &device_create_info, nullptr, &logical_device);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create logical device: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        // Retrieve the actual queue handles now that the logical device exists.
        // queueIndex 0 because we only requested 1 queue per family above.
        vkGetDeviceQueue(logical_device, queue_family_indices.graphics_family.value(), 0, &graphics_queue);
        vkGetDeviceQueue(logical_device, queue_family_indices.present_family.value(), 0, &present_queue);

        std::cout << "[Vulkan_Device] Logical device and queues created successfully.\n";
    }

    // ---------- Destructor ----------
    Vulkan_Device::~Vulkan_Device() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Device::Destroy() {
        // Note: physical_device is NOT destroyed - it's just a handle to
        // existing hardware, not something we created. Only the logical
        // device (our "connection" to that hardware) needs cleanup.
        if (logical_device != VK_NULL_HANDLE) {
            vkDestroyDevice(logical_device, nullptr);
        }
    }

    // ---------- Move constructor ----------
    Vulkan_Device::Vulkan_Device(Vulkan_Device&& _other) noexcept
        : physical_device(_other.physical_device),
        logical_device(_other.logical_device),
        graphics_queue(_other.graphics_queue),
        present_queue(_other.present_queue),
        queue_family_indices(_other.queue_family_indices),
        device_name(std::move(_other.device_name)) {

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

            _other.physical_device = VK_NULL_HANDLE;
            _other.logical_device = VK_NULL_HANDLE;
            _other.graphics_queue = VK_NULL_HANDLE;
            _other.present_queue = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Get_physical_device_handle ----------
    VkPhysicalDevice Vulkan_Device::Get_physical_device_handle() const {
        assert(physical_device != VK_NULL_HANDLE && "Get_physical_device_handle() called on a moved-from Vulkan_Device");
        return physical_device;
    }

    // ---------- Get_logical_device_handle ----------
    VkDevice Vulkan_Device::Get_logical_device_handle() const {
        assert(logical_device != VK_NULL_HANDLE && "Get_logical_device_handle() called on a moved-from Vulkan_Device");
        return logical_device;
    }

    // ---------- Get_graphics_queue ----------
    VkQueue Vulkan_Device::Get_graphics_queue() const {
        assert(graphics_queue != VK_NULL_HANDLE && "Get_graphics_queue() called on a moved-from Vulkan_Device");
        return graphics_queue;
    }

    // ---------- Get_present_queue ----------
    VkQueue Vulkan_Device::Get_present_queue() const {
        assert(present_queue != VK_NULL_HANDLE && "Get_present_queue() called on a moved-from Vulkan_Device");
        return present_queue;
    }

    // ---------- Get_queue_family_indices ----------
    const Queue_Family_Indices& Vulkan_Device::Get_queue_family_indices() const {
        assert(logical_device != VK_NULL_HANDLE && "Get_queue_family_indices() called on a moved-from Vulkan_Device");
        return queue_family_indices;
    }

    // ---------- Get_device_name ----------
    const std::string& Vulkan_Device::Get_device_name() const {
        assert(logical_device != VK_NULL_HANDLE && "Get_device_name() called on a moved-from Vulkan_Device");
        return device_name;
    }
    bool Vulkan_Device::Is_swapchain_maintenance1_enabled() const {
        return swapchain_maintenance1_enabled;
    }
    // ---------- Enumerate_physical_devices ----------
    std::vector<VkPhysicalDevice> Vulkan_Device::Enumerate_physical_devices(VkInstance _instance) const {
        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(_instance, &device_count, nullptr);

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(_instance, &device_count, devices.data());

        return devices;
    }
    // ---------- Find_supported_format ----------
    VkFormat Vulkan_Device::Find_supported_format(
        const std::vector<VkFormat>& _candidates,
        VkImageTiling _tiling,
        VkFormatFeatureFlags _features) const {

        assert(physical_device != VK_NULL_HANDLE && "Find_supported_format() called on a moved-from Vulkan_Device");
        assert(!_candidates.empty() && "Find_supported_format() called with an empty candidate list");

        // Try each candidate format in order, returning the first one
        // that supports the features we need with the given tiling mode.
        for (VkFormat format : _candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);

            // linearTilingFeatures and optimalTilingFeatures report what
            // operations this format supports depending on how the image
            // is laid out in memory - we check whichever one matches the
            // tiling mode the caller asked for.
            if (_tiling == VK_IMAGE_TILING_LINEAR &&
                (properties.linearTilingFeatures & _features) == _features) {
                return format;
            }

            if (_tiling == VK_IMAGE_TILING_OPTIMAL &&
                (properties.optimalTilingFeatures & _features) == _features) {
                return format;
            }
        }

        throw std::runtime_error("Failed to find a supported format among the given candidates");
    }
    bool Vulkan_Device::Is_swapchain_maintenance1_supported() const
    {
        assert(physical_device != VK_NULL_HANDLE && "Is_swapchain_maintenance1_supported() called on a moved-from Vulkan_Device");

        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);

        std::vector<VkExtensionProperties> available_extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, available_extensions.data());

        for (const auto& extension : available_extensions) {
            if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0) {
                return true;
            }
        }
        return false;
    }
    // ---------- Find_supported_depth_format ----------
    VkFormat Vulkan_Device::Find_supported_depth_format() const {
        assert(physical_device != VK_NULL_HANDLE && "Find_supported_depth_format() called on a moved-from Vulkan_Device");

        // Try formats in order of preference:
        // - D32_SFLOAT: 32-bit float depth, no stencil - highest precision,
        //   widely supported on modern GPUs, preferred when stencil isn't needed.
        // - D32_SFLOAT_S8_UINT: same depth precision, plus an 8-bit stencil
        //   channel - useful if you'll need stencil testing later (outlines,
        //   portals, etc.) without having to change formats.
        // - D24_UNORM_S8_UINT: 24-bit depth + 8-bit stencil, packed into 32
        //   bits - the most universally supported fallback on older/budget GPUs.
        return Find_supported_format(
            { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
        );
    }
    // ---------- Find_memory_type ----------
    uint32_t Vulkan_Device::Find_memory_type(uint32_t _type_filter, VkMemoryPropertyFlags _properties) const
    {
        assert(physical_device != VK_NULL_HANDLE && "Find_memory_type() called on a moved-from Vulkan_Device");

        // Query all memory types this GPU exposes, grouped into heaps
        // (e.g. one heap for GPU-local memory, one for memory shared
        // with the CPU/system RAM).
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            // _type_filter is a bitmask where bit i being set means
            // "memory type i is acceptable for this resource". We check
            // if this specific memory type index is allowed by the filter.
            bool type_is_allowed = (_type_filter & (1 << i)) != 0;

            // We also need this memory type to have ALL the properties
            // we asked for (e.g. DEVICE_LOCAL_BIT for fast GPU memory).
            bool has_required_properties =
                (memory_properties.memoryTypes[i].propertyFlags & _properties) == _properties;

            if (type_is_allowed && has_required_properties) {
                return i;
            }
        }

        throw std::runtime_error("Failed to find a suitable GPU memory type for the given requirements");
    }
    // ---------- Is_device_suitable ----------
    bool Vulkan_Device::Is_device_suitable(VkPhysicalDevice _device, VkSurfaceKHR _surface) const
    {
        assert(_device != VK_NULL_HANDLE && "Is_device_suitable() called with a null physical device");
        assert(_surface != VK_NULL_HANDLE && "Is_device_suitable() called with a null surface");

        Queue_Family_Indices indices = Find_queue_families(_device, _surface);

        bool extensions_supported = Check_device_extension_support(_device);

        bool swapchain_adequate = false;
        if (extensions_supported) {
            // We only check that AT LEAST one format and one present mode
            // exist - the actual best choice among them is Vulkan_Swapchain's
            // responsibility, not Vulkan_Device's.
            uint32_t format_count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(_device, _surface, &format_count, nullptr);

            uint32_t present_mode_count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(_device, _surface, &present_mode_count, nullptr);

            swapchain_adequate = (format_count > 0) && (present_mode_count > 0);
        }

        return indices.Is_complete() && extensions_supported && swapchain_adequate;
    }

    // ---------- Check_device_extension_support ----------
    bool Vulkan_Device::Check_device_extension_support(VkPhysicalDevice _device) const
    {
        assert(_device != VK_NULL_HANDLE && "Check_device_extension_support() called with a null physical device");
        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(_device, nullptr, &extension_count, nullptr);

        std::vector<VkExtensionProperties> available_extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(_device, nullptr, &extension_count, available_extensions.data());

        for (const char* required_extension : required_device_extensions) {
            bool found = false;

            for (const auto& available_extension : available_extensions) {
                if (std::strcmp(required_extension, available_extension.extensionName) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                return false;
            }
        }

        return true;
    }

    // ---------- Find_queue_families ----------
    Queue_Family_Indices Vulkan_Device::Find_queue_families(VkPhysicalDevice _device, VkSurfaceKHR _surface) const
    {
        assert(_device != VK_NULL_HANDLE && "Find_queue_families() called with a null physical device");
        assert(_surface != VK_NULL_HANDLE && "Find_queue_families() called with a null surface");

        Queue_Family_Indices indices;

        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(_device, &queue_family_count, nullptr);

        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(_device, &queue_family_count, queue_families.data());

        for (uint32_t i = 0; i < queue_family_count; ++i) {
            // Check if this family supports graphics operations
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphics_family = i;
            }

            // Check if this family supports presenting to our specific surface.
            // This is a separate capability from graphics - some GPUs only
            // support presentation on a different queue family.
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(_device, i, _surface, &present_support);

            if (present_support == VK_TRUE) {
                indices.present_family = i;
            }

            // Early exit once we've found everything we need - no point
            // checking remaining families.
            if (indices.Is_complete()) {
                break;
            }
        }

        return indices;
    }

    // ---------- Rate_device_suitability ----------
    uint32_t Vulkan_Device::Rate_device_suitability(VkPhysicalDevice _device, VkSurfaceKHR _surface) const
    {
        assert(_device != VK_NULL_HANDLE && "Rate_device_suitability() called with a null physical device");
        assert(_surface != VK_NULL_HANDLE && "Rate_device_suitability() called with a null surface");

        if (!Is_device_suitable(_device, _surface)) {
            return 0;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device, &properties);

        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(_device, &memory_properties);

        uint32_t score = 0;

        // Strongly prefer discrete GPUs over integrated ones - integrated
        // GPUs share system RAM and are typically far less powerful.
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        }
        else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 100;
        }

        // Higher maximum texture size generally correlates with a more
        // capable GPU - add it directly to the score as a tiebreaker.
        score += properties.limits.maxImageDimension2D;

        // Add available device-local (GPU) memory as part of the score,
        // scaled down since raw byte counts would otherwise dominate
        // the discrete/integrated bonus above.
        for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i) {
            if (memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                score += static_cast<uint32_t>(memory_properties.memoryHeaps[i].size / (1024 * 1024)); // MB
            }
        }

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

        // device_name is mutable here even though this function is const,
        // because we're caching it for later retrieval via Get_device_name() -
        // this is a deliberate exception, not an oversight. We use const_cast
        // since the member itself isn't declared mutable.
        const_cast<Vulkan_Device*>(this)->device_name = properties.deviceName;

        std::cout << "[Vulkan_Device] Selected GPU: " << properties.deviceName << "\n";
        std::cout << "[Vulkan_Device] Driver version: " << properties.driverVersion << "\n";
        std::cout << "[Vulkan_Device] Vulkan API version: "
            << VK_API_VERSION_MAJOR(properties.apiVersion) << "."
            << VK_API_VERSION_MINOR(properties.apiVersion) << "."
            << VK_API_VERSION_PATCH(properties.apiVersion) << "\n";
    }

}