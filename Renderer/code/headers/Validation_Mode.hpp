#pragma once

#include <cstdint>

namespace Renderer_System
{

    // Validation options of the Renderer. Kept apart from Vulkan_Instance so
    // that code which only chooses the level (EngineCore) does not depend
    // on any Vulkan header.

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

} // namespace Renderer_System
