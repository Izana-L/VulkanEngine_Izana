#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Renderer_System
{
    // What the Renderer demands of the instance and the GPU, as plain data.
    // Vulkan_Instance and Vulkan_Device used to read these numbers from
    // Renderer headers (Descriptor_Sets.hpp, Vulkan_Vertex_Layout.hpp,
    // Validation_Mode.hpp). Now the Renderer fills this struct and passes it
    // down, so the layer below does not need to know which descriptor layout
    // or vertex formats it is being asked to support.

    // Validation level requested at instance creation. Same levels as
    // Validation_Mode, which is the public one and stays in the Renderer
    // (EngineCore chooses it); the Renderer converts it when it builds
    // Device_Requirements. See Validation_Mode.hpp for what each level
    // checks and what it costs.
    enum class Validation_Level : uint8_t
    {
        Off,
        Standard,
        Gpu_Assisted
    };

    struct Device_Requirements
    {
        Validation_Level validation = Validation_Level::Off;

        // Descriptor set slots every pipeline layout binds
        // (VkPhysicalDeviceLimits::maxBoundDescriptorSets). A GPU that
        // reports fewer is not selected, and GPU-assisted validation is
        // refused when the slot it reserves would leave a GPU with fewer.
        uint32_t min_bound_descriptor_sets = 0;

        // Storage buffer descriptors one shader stage may access across all
        // the sets of a pipeline layout, and a pipeline layout may hold in
        // total (maxPerStageDescriptorStorageBuffers and
        // maxDescriptorSetStorageBuffers). The specification only
        // guarantees 4 and 24.
        uint32_t min_storage_buffers = 0;

        // Formats that Vulkan does not require for vertex buffers but the
        // vertex layouts of the Renderer use. A GPU that cannot read every
        // one of them from a vertex buffer is not selected.
        std::vector<VkFormat> vertex_formats;
    };

} // namespace Renderer_System