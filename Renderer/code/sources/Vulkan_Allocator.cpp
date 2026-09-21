#include <Vulkan_Allocator.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>

namespace Renderer_System
{
    Vulkan_Allocator::Vulkan_Allocator(const Vulkan_Instance& _instance,
        const Vulkan_Device& _device)
        : allocator(VK_NULL_HANDLE)
    {
        VmaAllocatorCreateInfo allocator_info{};
        allocator_info.physicalDevice = _device.Get_physical_device_handle();
        allocator_info.device = _device.Get_logical_device_handle();
        allocator_info.instance = _instance.Get_handle();

        // CRITICAL: must match what the instance was ACTUALLY created with,
        // not what we requested. Vulkan_Instance::Determine_api_version()
        // silently falls back on older drivers, and telling VMA 1.3 on a
        // 1.1 instance makes it call functions that don't exist.
        // Leaving this at 0 would make VMA assume 1.0 and lose the
        // dedicated-allocation / memory-budget extensions.
        allocator_info.vulkanApiVersion = _instance.Get_api_version();

        // pVulkanFunctions is left null on purpose: the Game project links
        // vulkan-1.lib statically, so VMA's default static function lookup
        // resolves everything. Only volk / VK_NO_PROTOTYPES setups need to
        // fill this in.

        VkResult result = vmaCreateAllocator(&allocator_info, &allocator);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create VMA allocator: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        std::cout << "[Vulkan_Allocator] Created.\n";
    }

    Vulkan_Allocator::~Vulkan_Allocator() { Destroy(); }

    void Vulkan_Allocator::Destroy()
    {
        if (allocator != VK_NULL_HANDLE) {
            // Asserts in debug builds if any allocation is still alive —
            // this is your leak detector, don't silence it.
            vmaDestroyAllocator(allocator);
            allocator = VK_NULL_HANDLE;
        }
    }

    void Vulkan_Allocator::Log_memory_budget() const
    {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(allocator, budgets);

        VkPhysicalDeviceMemoryProperties memory_properties{};
        // (or cache the heap count in Vulkan_Device)
        for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; ++i) {
            if (budgets[i].budget == 0) continue;
            std::cout << "[VMA] Heap " << i << ": "
                << (budgets[i].usage / (1024 * 1024)) << " MB used / "
                << (budgets[i].budget / (1024 * 1024)) << " MB budget\n";
        }
    }

    // Move ctor / assignment: same pattern as Vulkan_Device — steal the
    // handle and null the source so only one object destroys it.
    Vulkan_Allocator::Vulkan_Allocator(Vulkan_Allocator&& _other) noexcept
        : allocator(_other.allocator)
    {
        _other.allocator = VK_NULL_HANDLE;
    }

    Vulkan_Allocator& Vulkan_Allocator::operator=(Vulkan_Allocator&& _other) noexcept
    {
        if (this != &_other) {
            Destroy();
            allocator = _other.allocator;
            _other.allocator = VK_NULL_HANDLE;
        }
        return *this;
    }
}