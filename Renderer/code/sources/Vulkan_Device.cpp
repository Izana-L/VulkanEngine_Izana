#include <Vulkan_Device.hpp>
#include <Vulkan_Utils.hpp>
#include <Vulkan_Vertex_Layout.hpp>
#include <Descriptor_Sets.hpp>
#include <stdexcept>
#include <iostream>
#include <set>
#include <cstring>
#include <cassert>
#include <unordered_set>

namespace Renderer_System {

    namespace 
    {
        const std::vector<const char*> required_device_extensions = 
        {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };

        std::unordered_set<std::string> Enumerate_device_extensions(VkPhysicalDevice _device)
        {
            uint32_t extension_count = 0;
            VK_CHECK(vkEnumerateDeviceExtensionProperties(_device, nullptr, &extension_count, nullptr),
                "Vulkan_Device: enumerate device extensions");

            std::vector<VkExtensionProperties> available_extensions(extension_count);
            VK_CHECK(vkEnumerateDeviceExtensionProperties(_device, nullptr, &extension_count, available_extensions.data()),
                "Vulkan_Device: enumerate device extensions");

            std::unordered_set<std::string> names;
            for (const VkExtensionProperties& ext : available_extensions)
                names.insert(ext.extensionName);

            return names;
        }
    }

    // ---------- Constructor ----------
    Vulkan_Device::Vulkan_Device(const Vulkan_Instance& _instance, const Vulkan_Surface& _surface)
                                : physical_device(VK_NULL_HANDLE),
                                  logical_device(VK_NULL_HANDLE),
                                  graphics_queue(VK_NULL_HANDLE),
                                  present_queue(VK_NULL_HANDLE),
                                  device_name(),
                                  swapchain_maintenance1_enabled(false),
                                  sampler_anisotropy_enabled(false),
                                  max_sampler_anisotropy(1.0f)
    {
        VkInstance instance_handle = _instance.Get_handle();
        VkSurfaceKHR surface_handle = _surface.Get_handle();

        std::vector<VkPhysicalDevice> available_devices = Enumerate_physical_devices(instance_handle);
            
        if (available_devices.empty())
            throw std::runtime_error("No GPUs with Vulkan support found on this system");

        uint32_t best_score = 0;
        VkPhysicalDevice best_device = VK_NULL_HANDLE;
        Device_Support best_support;

        for (VkPhysicalDevice candidate : available_devices) {
            const Device_Support support = Query_device_support(candidate, surface_handle, _instance);
            const uint32_t score = Rate_device_suitability(candidate, support);
            if (score > best_score) {
                best_score = score;
                best_device = candidate;
                best_support = support;
            }
        }

        if (best_device == VK_NULL_HANDLE)
            throw std::runtime_error("No suitable GPU found (Vulkan 1.3, a present-capable queue, "
                "VK_KHR_swapchain, descriptor indexing features and at least " +
                std::to_string(Descriptor_Set::Count) + " bindable descriptor sets are required)");
                
        physical_device = best_device;
        queue_family_indices = best_support.queue_families;

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

        // ── Core features ─────────────────────────────────────────
        // Only features reported as supported are enabled; enabling an
        // unsupported one makes vkCreateDevice fail.
        VkPhysicalDeviceFeatures device_features{};
        device_features.samplerAnisotropy = best_support.sampler_anisotropy ? VK_TRUE : VK_FALSE;

        sampler_anisotropy_enabled = best_support.sampler_anisotropy;
        max_sampler_anisotropy = best_support.max_sampler_anisotropy;

        std::cout << "[Vulkan_Device] Sampler anisotropy "
            << (sampler_anisotropy_enabled ? "enabled (max " + std::to_string(max_sampler_anisotropy) + ")" : "not available")
            << ".\n";

        // ── Descriptor limits ─────────────────────────────────────
        bindless_limits = best_support.bindless_limits;

        std::cout << "[Vulkan_Device] Bindless limits (update-after-bind): sampled images "
            << bindless_limits.max_per_stage_sampled_images << "/stage, "
            << bindless_limits.max_per_set_sampled_images << "/set; samplers "
            << bindless_limits.max_per_stage_samplers << "/stage, "
            << bindless_limits.max_per_set_samplers << "/set; resources "
            << bindless_limits.max_per_stage_resources << "/stage; descriptors in all pools "
            << bindless_limits.max_descriptors_in_all_pools << ".\n";

        std::cout << "[Vulkan_Device] Bindable descriptor sets: " << best_support.max_bound_descriptor_sets
            << " (the engine uses " << Descriptor_Set::Count << ").\n";

        // GPU-AV binds a descriptor set of its own in the highest slot the
        // device reports. With exactly Descriptor_Set::Count slots that
        // slot may be the bindless set; the layer's own messages then say
        // whether it could instrument the shaders.
        if (_instance.Is_gpu_assisted_validation_enabled() &&
            best_support.max_bound_descriptor_sets == Descriptor_Set::Count) {
            std::cerr << "[Vulkan_Device] GPU-assisted validation is on, but maxBoundDescriptorSets equals the "
                << Descriptor_Set::Count << " sets the engine uses: GPU-AV may have no free slot.\n";
        }

        // ── Extensions ────────────────────────────────────────────
        std::vector<const char*> device_extensions = required_device_extensions;

        // The device half of swapchain maintenance1 needs all three: the
        // instance half (surface maintenance1 + surface capabilities2),
        // the device extension, and the feature bit.
        swapchain_maintenance1_enabled =
            _instance.Is_surface_maintenance1_enabled() &&
            best_support.swapchain_maintenance1_extension != nullptr &&
            best_support.swapchain_maintenance1_feature;

        if (swapchain_maintenance1_enabled) {
            device_extensions.push_back(best_support.swapchain_maintenance1_extension);
            std::cout << "[Vulkan_Device] " << best_support.swapchain_maintenance1_extension
                << " enabled: present fences available.\n";
        }
        else {
            std::cout << "[Vulkan_Device] Swapchain maintenance1 not enabled ("
                << (_instance.Is_surface_maintenance1_enabled() ? "" : "instance half missing; ")
                << (best_support.swapchain_maintenance1_extension ? "" : "device extension missing; ")
                << (best_support.swapchain_maintenance1_feature ? "" : "feature unsupported; ")
                << "falling back to per-image semaphores only).\n";
        }

        // ── Feature structs (pNext chain) ─────────────────────────
        VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing_features{};
        descriptor_indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

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

        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance1_features{};
        swapchain_maintenance1_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
        swapchain_maintenance1_features.swapchainMaintenance1 = VK_TRUE;

        // Chain: descriptor_indexing -> [swapchain_maintenance1] -> null.
        // The maintenance1 struct is chained only when its extension is
        // enabled; chaining a feature struct of a disabled extension is
        // invalid usage.
        void* chain_head = &descriptor_indexing_features;
        descriptor_indexing_features.pNext = swapchain_maintenance1_enabled ? &swapchain_maintenance1_features : nullptr;

        std::cout << "[Vulkan_Device] Bindless descriptor indexing enabled.\n";

        // ── Device create info ────────────────────────────────────
        VkDeviceCreateInfo device_create_info{};
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.pNext = chain_head;
        device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
        device_create_info.pQueueCreateInfos = queue_create_infos.data();
        device_create_info.pEnabledFeatures = &device_features;
        device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_create_info.ppEnabledExtensionNames = device_extensions.data();

        VK_CHECK(vkCreateDevice(physical_device, &device_create_info, nullptr, &logical_device),
            "Failed to create logical device");

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
        if (logical_device != VK_NULL_HANDLE) {
            vkDestroyDevice(logical_device, nullptr);
            logical_device = VK_NULL_HANDLE;
        }
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
        sampler_anisotropy_enabled(_other.sampler_anisotropy_enabled),
        max_sampler_anisotropy(_other.max_sampler_anisotropy),
        bindless_limits(_other.bindless_limits)
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
            sampler_anisotropy_enabled = _other.sampler_anisotropy_enabled;
            max_sampler_anisotropy = _other.max_sampler_anisotropy;
            bindless_limits = _other.bindless_limits;

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
        return device_name;
    }

    bool Vulkan_Device::Is_swapchain_maintenance1_enabled() const {
        return swapchain_maintenance1_enabled;
    }

    bool Vulkan_Device::Is_bindless_supported() const {
        return logical_device != VK_NULL_HANDLE;
    }

    bool Vulkan_Device::Is_sampler_anisotropy_enabled() const {
        return sampler_anisotropy_enabled;
    }

    float Vulkan_Device::Get_max_sampler_anisotropy() const {
        return max_sampler_anisotropy;
    }

    const Bindless_Limits& Vulkan_Device::Get_bindless_limits() const {
        return bindless_limits;
    }

    // ---------- Enumerate_physical_devices ----------
    std::vector<VkPhysicalDevice> Vulkan_Device::Enumerate_physical_devices(
        VkInstance _instance) const
    {
        uint32_t device_count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(_instance, &device_count, nullptr),
            "Vulkan_Device: enumerate physical devices");

        std::vector<VkPhysicalDevice> devices(device_count);
        VK_CHECK(vkEnumeratePhysicalDevices(_instance, &device_count, devices.data()),
            "Vulkan_Device: enumerate physical devices");
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

    // ---------- Query_device_support ----------
    Device_Support Vulkan_Device::Query_device_support(VkPhysicalDevice _device,VkSurfaceKHR _surface,const Vulkan_Instance& _instance) const
    {
        assert(_device != VK_NULL_HANDLE);
        assert(_surface != VK_NULL_HANDLE);

        Device_Support support;

        // Properties: API version and limits.
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device, &properties);
        support.api_version = properties.apiVersion;
        support.max_sampler_anisotropy = properties.limits.maxSamplerAnisotropy;
        support.max_bound_descriptor_sets = properties.limits.maxBoundDescriptorSets;

        // Descriptor indexing limits. The properties struct is core only
        // from Vulkan 1.2, and chaining it on an older device is invalid;
        // such devices keep all-zero limits and are rejected anyway.
        if (properties.apiVersion >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceDescriptorIndexingProperties indexing_properties{};
            indexing_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;

            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &indexing_properties;

            vkGetPhysicalDeviceProperties2(_device, &properties2);

            Bindless_Limits& limits = support.bindless_limits;
            limits.max_per_stage_sampled_images = indexing_properties.maxPerStageDescriptorUpdateAfterBindSampledImages;
            limits.max_per_set_sampled_images = indexing_properties.maxDescriptorSetUpdateAfterBindSampledImages;
            limits.max_per_stage_samplers = indexing_properties.maxPerStageDescriptorUpdateAfterBindSamplers;
            limits.max_per_set_samplers = indexing_properties.maxDescriptorSetUpdateAfterBindSamplers;
            limits.max_per_stage_resources = indexing_properties.maxPerStageUpdateAfterBindResources;
            limits.max_descriptors_in_all_pools = indexing_properties.maxUpdateAfterBindDescriptorsInAllPools;
        }

        // Extensions.
        const std::unordered_set<std::string> extensions = Enumerate_device_extensions(_device);

        support.swapchain_extension = true;
        for (const char* required : required_device_extensions)
            if (extensions.count(required) == 0) support.swapchain_extension = false;

        if (extensions.count(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
            support.swapchain_maintenance1_extension = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        else if (extensions.count(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
            support.swapchain_maintenance1_extension = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;

        // Features, through one chained query. The swapchain maintenance1
        // struct is chained only when the extension exists (and its
        // instance half is present), as required for a feature query.
        VkPhysicalDeviceDescriptorIndexingFeatures indexing_features{};
        indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance1_features{};
        maintenance1_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;

        const bool query_maintenance1 = support.swapchain_maintenance1_extension != nullptr && _instance.Is_surface_maintenance1_enabled();

        indexing_features.pNext = query_maintenance1 ? &maintenance1_features : nullptr;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &indexing_features;

        vkGetPhysicalDeviceFeatures2(_device, &features2);

        support.sampler_anisotropy = features2.features.samplerAnisotropy == VK_TRUE;
        support.vertex_formats = true;
        for (VkFormat format : Vulkan_Vertex_Layout::OPTIONAL_VERTEX_FORMATS)
        {
            VkFormatProperties format_properties{};
            vkGetPhysicalDeviceFormatProperties(_device, format, &format_properties);

            if ((format_properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0)
                support.vertex_formats = false;
        }
        support.bindless = indexing_features.runtimeDescriptorArray == VK_TRUE &&
                           indexing_features.descriptorBindingPartiallyBound == VK_TRUE &&
                           indexing_features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
                           indexing_features.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
           

        support.swapchain_maintenance1_feature =
            query_maintenance1 && maintenance1_features.swapchainMaintenance1 == VK_TRUE;

        // Queues and surface.
        support.queue_families = Find_queue_families(_device, _surface);

        if (support.swapchain_extension) {
            uint32_t format_count = 0;
            VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(_device, _surface, &format_count, nullptr),
                "Vulkan_Device: query surface formats");

            uint32_t present_mode_count = 0;
            VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(_device, _surface, &present_mode_count, nullptr),
                "Vulkan_Device: query surface present modes");

            support.surface_adequate = (format_count > 0) && (present_mode_count > 0);
        }

        return support;
    }

    // ---------- Is_device_suitable ----------
    bool Vulkan_Device::Is_device_suitable(const Device_Support& _support) const
    {
        // Extended dynamic state (vkCmdSetCullMode, vkCmdSetDepthTestEnable, ...)
        // is core AND required in Vulkan 1.3 - no feature bit, no extension.
        // Vulkan_Instance::Determine_api_version caps the *instance* version;
        // this is the *device* version, which is what actually gates those
        // entry points.
        const bool api_1_3_supported = _support.api_version >= VK_API_VERSION_1_3;

        // Every pipeline layout declares Descriptor_Set::Count sets (0-3).
        // The spec guarantees at least 4, so this only rejects devices
        // that report fewer, e.g. when a validation layer reserves a slot.
        const bool enough_descriptor_sets = _support.max_bound_descriptor_sets >= Descriptor_Set::Count;

        // Bindless textures are not optional in this renderer: every
        // pipeline layout carries the bindless set and the fragment shader
        // indexes it. A device that cannot do it is not selected, so
        // Bindless_Registry never has to run on a device without support.
        return api_1_3_supported && _support.queue_families.Is_complete()&& _support.swapchain_extension && 
                                   _support.surface_adequate && _support.bindless && _support.vertex_formats && enough_descriptor_sets;
            
            
            
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
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(_device, i, _surface, &present_support),
                "Vulkan_Device: query surface support");

            if (present_support == VK_TRUE)
                indices.present_family = i;

            if (indices.Is_complete()) break;
        }

        return indices;
    }

    // ---------- Rate_device_suitability ----------
    uint32_t Vulkan_Device::Rate_device_suitability(
        VkPhysicalDevice _device, const Device_Support& _support) const
    {
        assert(_device != VK_NULL_HANDLE);

        if (!Is_device_suitable(_support)) return 0;

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

        // Never zero for a suitable device, so a suitable device always
        // beats "no device".
        return score + 1;
    }

    // ---------- Log_selected_device ----------
    void Vulkan_Device::Log_selected_device(VkPhysicalDevice _device) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device, &properties);

        device_name = properties.deviceName;

        std::cout << "[Vulkan_Device] Selected GPU: " << properties.deviceName << "\n";
        std::cout << "[Vulkan_Device] Driver version: " << properties.driverVersion << "\n";
        std::cout << "[Vulkan_Device] Vulkan API version: "
            << VK_API_VERSION_MAJOR(properties.apiVersion) << "."
            << VK_API_VERSION_MINOR(properties.apiVersion) << "."
            << VK_API_VERSION_PATCH(properties.apiVersion) << "\n";
    }

}
