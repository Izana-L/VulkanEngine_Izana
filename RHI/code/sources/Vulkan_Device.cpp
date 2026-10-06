#include <Vulkan_Device.hpp>
#include <Vulkan_Utils.hpp>
#include <algorithm>
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

        // Every requirement of Is_device_suitable that _support fails, in
        // words, for the error raised when no GPU qualifies. Empty when the
        // device is suitable; Is_device_suitable is defined as exactly that,
        // so the message and the decision cannot disagree.
        // First floating-point depth format of the candidates that can be a
        // depth attachment with optimal tiling, in order of preference:
        // D32_SFLOAT first, because the stencil aspect is not used and
        // costs memory. VK_FORMAT_UNDEFINED when none qualifies.
        VkFormat Select_float_depth_format(VkPhysicalDevice _device)
        {
            constexpr VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT };

            for (VkFormat format : candidates)
            {
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(_device, format, &properties);

                if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
                    return format;
            }

            return VK_FORMAT_UNDEFINED;
        }

        std::vector<std::string> Find_missing_requirements(const Device_Support& _support, const Device_Requirements& _requirements)
        {
            std::vector<std::string> missing;

            // Extended dynamic state (vkCmdSetCullMode, vkCmdSetFrontFace,
            // vkCmdSetDepthTestEnable, ...) is core AND required in Vulkan
            // 1.3 - no feature bit, no extension. The functionality
            // available to the application is bounded by both versions,
            // the instance's and the device's, so api_version is their
            // minimum (Query_device_support). The instance is always
            // created with 1.3, which leaves the device version as the
            // deciding one, but the check does not depend on it.
            if (_support.api_version < VK_API_VERSION_1_3)
                missing.push_back("Vulkan 1.3");

            if (!_support.queue_families.graphics_family.has_value())
                missing.push_back("a queue family with graphics and compute");

            if (!_support.queue_families.present_family.has_value())
                missing.push_back("a queue family that can present to the window surface");

            if (!_support.swapchain_extension)
                missing.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            else if (!_support.surface_adequate)
                missing.push_back("a surface format and a present mode for the window");

            // Bindless textures are not optional in this renderer: every
            // pipeline layout carries the bindless set and the fragment
            // shader indexes it. A device that cannot do it is not
            // selected, so Bindless_Registry never has to run on a device
            // without support.
            if (!_support.bindless)
                missing.push_back("the descriptor indexing features of bindless textures");

            // GPU-driven drawing: the opaque pass is recorded as indirect
            // draws whose firstInstance carries the object index, and the
            // culling pass decides the draw count on the GPU.
            if (!_support.multi_draw_indirect)
                missing.push_back("multiDrawIndirect");

            if (!_support.draw_indirect_first_instance)
                missing.push_back("drawIndirectFirstInstance");

            if (!_support.draw_indirect_count)
                missing.push_back("drawIndirectCount");

            // The progress of the frames in flight is tracked by one timeline
            // semaphore that the Renderer waits on and reads.
            if (!_support.timeline_semaphore)
                missing.push_back("timelineSemaphore");

            // The reverse-Z projection with an infinite far plane needs a
            // floating-point depth buffer; a GPU that only offers UNORM
            // depth is rejected here, where another GPU can still be chosen.
            if (_support.depth_format == VK_FORMAT_UNDEFINED)
                missing.push_back("a floating-point depth format (D32_SFLOAT or D32_SFLOAT_S8_UINT) usable as a depth attachment");

            // Weighted blended OIT: the transparent pipeline blends its two
            // color attachments differently.
            if (!_support.independent_blend)
                missing.push_back("independentBlend");

            if (!_support.vertex_formats)
                missing.push_back("the vertex buffer formats the renderer's vertex layouts use");

            if (_support.max_per_stage_storage_buffers < _requirements.min_storage_buffers ||
                _support.max_set_storage_buffers < _requirements.min_storage_buffers)
            {
                missing.push_back(std::to_string(_requirements.min_storage_buffers) + " storage buffers per stage and per pipeline layout (reports " +
                                  std::to_string(_support.max_per_stage_storage_buffers) + " and " +
                                  std::to_string(_support.max_set_storage_buffers) + ")");
            }

            // Every pipeline layout binds min_bound_descriptor_sets sets.
            // The specification guarantees at least 4. The value is the one
            // the instance reports: with GPU-assisted validation, the layer
            // reports one slot less than the device has, but Vulkan_Instance
            // only enables GPU-AV when no device would drop below
            // min_bound_descriptor_sets because of it.
            if (_support.max_bound_descriptor_sets < _requirements.min_bound_descriptor_sets)
                missing.push_back(std::to_string(_requirements.min_bound_descriptor_sets) + " bindable descriptor sets (reports " +
                                  std::to_string(_support.max_bound_descriptor_sets) + ")");

            return missing;
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
                                  max_sampler_anisotropy(1.0f),
                                  bindless_enabled(false),
                                  fill_mode_non_solid_enabled(false),
                                  max_draw_indirect_count(0),
                                  timestamp_valid_bits(0),
                                  timestamp_period(0.0f),
                                  depth_format(VK_FORMAT_UNDEFINED)
    {
        VkInstance instance_handle = _instance.Get_handle();
        VkSurfaceKHR surface_handle = _surface.Get_handle();

        // What the Renderer asked of the GPU. The instance keeps it, so the
        // checks of device selection and those of GPU-assisted validation
        // read the same values.
        const Device_Requirements& requirements = _instance.Get_requirements();

        std::vector<VkPhysicalDevice> available_devices = Enumerate_physical_devices(instance_handle);
            
        if (available_devices.empty())
            throw std::runtime_error("No GPUs with Vulkan support found on this system");

        std::optional<Device_Rank> best_rank;
        VkPhysicalDevice best_device = VK_NULL_HANDLE;
        Device_Support best_support;

        // What each rejected GPU lacks, so the error names the actual cause
        // instead of listing every requirement.
        std::string rejections;

        for (VkPhysicalDevice candidate : available_devices) {
            const Device_Support support = Query_device_support(candidate, surface_handle, _instance);
            const std::optional<Device_Rank> rank = Rate_device_suitability(candidate, support, requirements);
            if (rank && (!best_rank || *best_rank < *rank)) {
                best_rank = rank;
                best_device = candidate;
                best_support = support;
            }

            const std::vector<std::string> missing = Find_missing_requirements(support, requirements);
            if (!missing.empty()) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate, &properties);

                rejections += "\n  " + std::string(properties.deviceName) + " lacks: ";
                for (size_t i = 0; i < missing.size(); ++i)
                    rejections += (i == 0 ? "" : ", ") + missing[i];
            }
        }

        if (best_device == VK_NULL_HANDLE)
            throw std::runtime_error("No suitable GPU found." + rejections);
                
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

        // Required by device selection, so all three are supported here.
        device_features.multiDrawIndirect = VK_TRUE;
        device_features.drawIndirectFirstInstance = VK_TRUE;
        device_features.independentBlend = VK_TRUE;

        device_features.fillModeNonSolid = best_support.fill_mode_non_solid ? VK_TRUE : VK_FALSE;

        sampler_anisotropy_enabled = best_support.sampler_anisotropy;
        max_sampler_anisotropy = best_support.max_sampler_anisotropy;
        fill_mode_non_solid_enabled = best_support.fill_mode_non_solid;
        max_draw_indirect_count = best_support.max_draw_indirect_count;
        timestamp_valid_bits = best_support.timestamp_valid_bits;
        timestamp_period = best_support.timestamp_period;
        depth_format = best_support.depth_format;

        std::cout << "[Vulkan_Device] Depth format: " << Vulkan_Utils::Vk_format_to_string(depth_format) << ".\n";

        std::cout << "[Vulkan_Device] Sampler anisotropy "
            << (sampler_anisotropy_enabled ? "enabled (max " + std::to_string(max_sampler_anisotropy) + ")" : "not available")
            << ".\n";

        std::cout << "[Vulkan_Device] Indirect drawing enabled (multiDrawIndirect, drawIndirectFirstInstance, "
            "drawIndirectCount; maxDrawIndirectCount " << max_draw_indirect_count << "). independentBlend and "
            "timelineSemaphore enabled. fillModeNonSolid " << (fill_mode_non_solid_enabled ? "enabled" : "not available") << ".\n";

        if (timestamp_valid_bits > 0)
            std::cout << "[Vulkan_Device] Timestamps: " << timestamp_valid_bits << " valid bits, "
                      << timestamp_period << " ns per tick.\n";
        else
            std::cout << "[Vulkan_Device] Timestamps not supported on the graphics queue family: GPU timings disabled.\n";

        // ── Descriptor limits ─────────────────────────────────────
        bindless_limits = best_support.bindless_limits;

        std::cout << "[Vulkan_Device] Bindless limits (update-after-bind): sampled images "
            << bindless_limits.max_per_stage_sampled_images << "/stage, "
            << bindless_limits.max_per_set_sampled_images << "/set; samplers "
            << bindless_limits.max_per_stage_samplers << "/stage, "
            << bindless_limits.max_per_set_samplers << "/set; resources "
            << bindless_limits.max_per_stage_resources << "/stage; descriptors in all pools "
            << bindless_limits.max_descriptors_in_all_pools << ".\n";

        // With GPU-assisted validation the reported value already excludes
        // the slot the layer reserves for itself (it sits right above the
        // reported ones), so any value accepted by device selection leaves
        // GPU-AV its own slot; Vulkan_Instance guarantees no device drops
        // below min_bound_descriptor_sets because of it.
        std::cout << "[Vulkan_Device] Bindable descriptor sets: " << best_support.max_bound_descriptor_sets
            << (_instance.Is_gpu_assisted_validation_enabled() ? " (after the slot reserved by GPU-assisted validation)" : "")
            << " (the engine uses " << requirements.min_bound_descriptor_sets << ").\n";

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

        // ── Descriptor indexing ───────────────────────────────────
        // Recorded from the selected GPU instead of assumed:
        // Is_bindless_supported() returns this value, and the features
        // below are requested only when it is true, since enabling an
        // unsupported feature makes vkCreateDevice fail. Device selection
        // requires them today; if that ever changes, the device is created
        // without them and Bindless_Registry refuses to run on it.
        bindless_enabled = best_support.bindless;

        // ── Feature structs (pNext chain) ─────────────────────────
        // Vulkan 1.2 features go through VkPhysicalDeviceVulkan12Features,
        // the only structure that holds drawIndirectCount. The descriptor
        // indexing features live in it as well: chaining it together with
        // VkPhysicalDeviceDescriptorIndexingFeatures (or any other
        // structure it aggregates) is invalid usage, so the bindless
        // features are set here and that structure is no longer used.
        VkPhysicalDeviceVulkan12Features vulkan12_features{};
        vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

        const VkBool32 bindless_feature = bindless_enabled ? VK_TRUE : VK_FALSE;

        // Array size known only at runtime.
        vulkan12_features.runtimeDescriptorArray = bindless_feature;

        // Allow unbound slots in the array (not every texture slot
        // needs to be filled as long as the shader never accesses it).
        vulkan12_features.descriptorBindingPartiallyBound = bindless_feature;

        // Allow non-uniform indexing in shaders (each invocation can
        // use a different texture index).
        vulkan12_features.shaderSampledImageArrayNonUniformIndexing = bindless_feature;

        // Allow writing descriptors after the set was bound in a command
        // buffer that is still being recorded; the submission sees them.
        vulkan12_features.descriptorBindingSampledImageUpdateAfterBind = bindless_feature;

        // Allow writing descriptors that no pending command buffer reads
        // while frames that use the set are still executing, so bindless
        // slots can be registered, rewritten and released without waiting
        // for the device to go idle.
        vulkan12_features.descriptorBindingUpdateUnusedWhilePending = bindless_feature;

        // vkCmdDrawIndexedIndirectCount: the opaque pass draws as many
        // commands as the culling pass wrote. Required by device selection.
        vulkan12_features.drawIndirectCount = VK_TRUE;

        // Timeline semaphore of the frames in flight. Core in Vulkan 1.2,
        // but the feature has to be enabled. Required by device selection.
        vulkan12_features.timelineSemaphore = VK_TRUE;

        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance1_features{};
        swapchain_maintenance1_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
        swapchain_maintenance1_features.swapchainMaintenance1 = VK_TRUE;

        // Chain: vulkan12 -> [swapchain_maintenance1] -> null.
        // The maintenance1 struct is chained only when its extension is
        // enabled; chaining a feature struct of a disabled extension is
        // invalid usage.
        void* chain_head = &vulkan12_features;
        vulkan12_features.pNext = swapchain_maintenance1_enabled ? &swapchain_maintenance1_features : nullptr;

        std::cout << "[Vulkan_Device] Bindless descriptor indexing "
            << (bindless_enabled ? "enabled" : "not available") << ".\n";

        // ── Device create info ────────────────────────────────────
        VkDeviceCreateInfo device_create_info{};
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.pNext = chain_head;
        device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
        device_create_info.pQueueCreateInfos = queue_create_infos.data();
        device_create_info.pEnabledFeatures = &device_features;
        device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_create_info.ppEnabledExtensionNames = device_extensions.data();

        // GPU-AV checks the device features it needs during vkCreateDevice
        // and disables itself, with a message, when one is missing; the
        // instance records that message (Vulkan_Instance::Debug_callback).
        const bool gpu_av_before_device = _instance.Is_gpu_assisted_validation_enabled();

        VK_CHECK(vkCreateDevice(physical_device, &device_create_info, nullptr, &logical_device),
            "Failed to create logical device");

        if (gpu_av_before_device && !_instance.Is_gpu_assisted_validation_enabled()) {
            std::cerr << "[Vulkan_Device] The validation layer reported a GPU-assisted validation problem while "
                "creating the device (see its message above): GPU-AV checks may be inactive.\n";
        }

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
        bindless_enabled(_other.bindless_enabled),
        fill_mode_non_solid_enabled(_other.fill_mode_non_solid_enabled),
        max_draw_indirect_count(_other.max_draw_indirect_count),
        timestamp_valid_bits(_other.timestamp_valid_bits),
        timestamp_period(_other.timestamp_period),
        depth_format(_other.depth_format),
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
            bindless_enabled = _other.bindless_enabled;
            fill_mode_non_solid_enabled = _other.fill_mode_non_solid_enabled;
            max_draw_indirect_count = _other.max_draw_indirect_count;
            timestamp_valid_bits = _other.timestamp_valid_bits;
            timestamp_period = _other.timestamp_period;
            depth_format = _other.depth_format;
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
        return bindless_enabled;
    }

    bool Vulkan_Device::Is_sampler_anisotropy_enabled() const {
        return sampler_anisotropy_enabled;
    }

    float Vulkan_Device::Get_max_sampler_anisotropy() const {
        return max_sampler_anisotropy;
    }

    bool Vulkan_Device::Is_fill_mode_non_solid_enabled() const {
        return fill_mode_non_solid_enabled;
    }

    uint32_t Vulkan_Device::Get_max_draw_indirect_count() const {
        return max_draw_indirect_count;
    }

    uint32_t Vulkan_Device::Get_timestamp_valid_bits() const {
        return timestamp_valid_bits;
    }

    float Vulkan_Device::Get_timestamp_period() const {
        return timestamp_period;
    }

    VkFormat Vulkan_Device::Get_depth_format() const {
        return depth_format;
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

    // ---------- Query_device_support ----------
    Device_Support Vulkan_Device::Query_device_support(VkPhysicalDevice _device,VkSurfaceKHR _surface,const Vulkan_Instance& _instance) const
    {
        assert(_device != VK_NULL_HANDLE);
        assert(_surface != VK_NULL_HANDLE);

        Device_Support support;

        // Properties: API version and limits. The version the application
        // can rely on is the minimum of the instance's and the device's:
        // the device may report a higher one than the instance enables.
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device, &properties);
        support.api_version = std::min(_instance.Get_api_version(), properties.apiVersion);
        support.max_sampler_anisotropy = properties.limits.maxSamplerAnisotropy;
        support.max_bound_descriptor_sets = properties.limits.maxBoundDescriptorSets;
        support.max_draw_indirect_count = properties.limits.maxDrawIndirectCount;
        support.max_per_stage_storage_buffers = properties.limits.maxPerStageDescriptorStorageBuffers;
        support.max_set_storage_buffers = properties.limits.maxDescriptorSetStorageBuffers;
        support.timestamp_period = properties.limits.timestampPeriod;

        // Descriptor indexing limits. The properties struct is core only
        // from Vulkan 1.2, and chaining it on an older device is invalid;
        // such devices keep all-zero limits and are rejected anyway.
        if (support.api_version >= VK_API_VERSION_1_2) {
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
        // VkPhysicalDeviceVulkan12Features is chained only on a Vulkan 1.2
        // device, where the structure is defined; older devices keep every
        // Vulkan 1.2 feature false and are rejected anyway. It is the same
        // structure the device is created with, so the query and the
        // creation read the features from one place.
        VkPhysicalDeviceVulkan12Features vulkan12_features{};
        vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance1_features{};
        maintenance1_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;

        const bool query_vulkan12 = support.api_version >= VK_API_VERSION_1_2;
        const bool query_maintenance1 = support.swapchain_maintenance1_extension != nullptr && _instance.Is_surface_maintenance1_enabled();

        void* feature_chain = query_maintenance1 ? &maintenance1_features : nullptr;

        if (query_vulkan12)
        {
            vulkan12_features.pNext = feature_chain;
            feature_chain = &vulkan12_features;
        }

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = feature_chain;

        vkGetPhysicalDeviceFeatures2(_device, &features2);

        support.sampler_anisotropy = features2.features.samplerAnisotropy == VK_TRUE;
        support.multi_draw_indirect = features2.features.multiDrawIndirect == VK_TRUE;
        support.draw_indirect_first_instance = features2.features.drawIndirectFirstInstance == VK_TRUE;
        support.fill_mode_non_solid = features2.features.fillModeNonSolid == VK_TRUE;
        support.independent_blend = features2.features.independentBlend == VK_TRUE;
        support.draw_indirect_count = vulkan12_features.drawIndirectCount == VK_TRUE;
        support.timeline_semaphore = vulkan12_features.timelineSemaphore == VK_TRUE;
        support.depth_format = Select_float_depth_format(_device);
        support.vertex_formats = true;
        for (VkFormat format : _instance.Get_requirements().vertex_formats)
        {
            VkFormatProperties format_properties{};
            vkGetPhysicalDeviceFormatProperties(_device, format, &format_properties);

            if ((format_properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0)
                support.vertex_formats = false;
        }
        support.bindless = vulkan12_features.runtimeDescriptorArray == VK_TRUE &&
                           vulkan12_features.descriptorBindingPartiallyBound == VK_TRUE &&
                           vulkan12_features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
                           vulkan12_features.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
                           vulkan12_features.descriptorBindingUpdateUnusedWhilePending == VK_TRUE;


        support.swapchain_maintenance1_feature =
            query_maintenance1 && maintenance1_features.swapchainMaintenance1 == VK_TRUE;

        // Queues and surface.
        support.queue_families = Find_queue_families(_device, _surface);

        // Timestamp support belongs to the queue family that records the
        // frame: the graphics family, which also records every dispatch.
        if (support.queue_families.graphics_family.has_value())
        {
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(_device, &family_count, nullptr);

            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(_device, &family_count, families.data());

            const uint32_t graphics_family = support.queue_families.graphics_family.value();

            if (graphics_family < family_count)
                support.timestamp_valid_bits = families[graphics_family].timestampValidBits;
        }

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
    bool Vulkan_Device::Is_device_suitable(const Device_Support& _support, const Device_Requirements& _requirements) const
    {
        // The requirements and their reasons live in
        // Find_missing_requirements, which also words the error raised
        // when no device qualifies.
        return Find_missing_requirements(_support, _requirements).empty();
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

        // The graphics queue also records every compute dispatch, so its
        // family must support both kinds of work. Desktop GPUs always
        // expose such a family, but the specification only guarantees
        // that one family of one device of the implementation supports
        // graphics and compute together, not that every graphics family
        // does. A device without such a family ends up without
        // graphics_family and is rejected as incomplete.
        constexpr VkQueueFlags required_graphics_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

        for (uint32_t i = 0; i < queue_family_count; ++i) 
        {
            if ((queue_families[i].queueFlags & required_graphics_flags) == required_graphics_flags)
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
    std::optional<Device_Rank> Vulkan_Device::Rate_device_suitability(
        VkPhysicalDevice _device, const Device_Support& _support, const Device_Requirements& _requirements) const
    {
        assert(_device != VK_NULL_HANDLE);

        if (!Is_device_suitable(_support, _requirements)) return std::nullopt;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_device, &properties);

        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(_device, &memory_properties);

        Device_Rank rank;

        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            rank.type_level = 2;
        else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            rank.type_level = 1;

        for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i)
            if (memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                rank.device_local_bytes = std::max<uint64_t>(
                    rank.device_local_bytes, memory_properties.memoryHeaps[i].size);

        rank.vendor_id = properties.vendorID;
        rank.device_id = properties.deviceID;

        return rank;
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
