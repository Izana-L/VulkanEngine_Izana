#pragma once

#include <vulkan/vulkan.h>

// The attribute locations the shaders declare (mesh_vertex_input.glsl).
#include "../../shaders/common/gpu_shared.h"

#include <Vertex.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Renderer_System::Vulkan_Vertex_Layout
{

    // Vulkan_Vertex_Layout: translates CoreTypes vertex types into the
    // Vulkan input descriptors that a pipeline needs at creation time.
    //
    // Lives inside the Renderer: CoreTypes::Vertex has no knowledge of
    // Vulkan; this is the only place that bridges the two.
    // Called once per pipeline creation, not per frame.
    //
    // Usage:
    //   auto binding    = Vulkan_Vertex_Layout::Get_binding_description<CoreTypes::Vertex_Static_Mesh>();
    //   auto attributes = Vulkan_Vertex_Layout::Get_attribute_descriptions<CoreTypes::Vertex_Static_Mesh>();
    //
    // Only the GPU vertex types have a layout. Vertex_Static_Mesh_CPU
    // (what MeshData stores) has none, so passing it here fails to
    // compile instead of describing a buffer that does not exist.

    // =========================================================
    // Formats that need a device check
    // =========================================================

    // Normal and tangent format. Unlike every other format in this file,
    // Vulkan does not require it for vertex buffers, so Vulkan_Device
    // only selects a GPU that supports everything in
    // OPTIONAL_VERTEX_FORMATS. Desktop GPUs do.
    inline constexpr VkFormat NORMAL_TANGENT_FORMAT = VK_FORMAT_A2B10G10R10_SNORM_PACK32;

    inline constexpr std::array< VkFormat, 1 > OPTIONAL_VERTEX_FORMATS = { NORMAL_TANGENT_FORMAT };

    // =========================================================
    // Attribute_Count trait
    // Associates each vertex type with its attribute count at
    // compile time, so Get_attribute_descriptions can return a
    // correctly-sized std::array without resorting to auto.
    // =========================================================

    template< typename VERTEX_TYPE >
    struct Attribute_Count;

    template<> struct Attribute_Count< CoreTypes::Vertex_Simple > { static constexpr size_t value = 2; };
    template<> struct Attribute_Count< CoreTypes::Vertex_Static_Mesh > { static constexpr size_t value = 5; };
    template<> struct Attribute_Count< CoreTypes::Vertex_Skinned_Mesh > { static constexpr size_t value = 7; };

    // =========================================================
    // Get_binding_description
    // =========================================================

    // One binding (0), per-vertex rate, stride = sizeof(VERTEX_TYPE). The
    // same for every vertex type, so a single template serves them all;
    // the Attribute_Count specialization above is what restricts it to
    // the known vertex types.
    template< typename VERTEX_TYPE >
    inline VkVertexInputBindingDescription Get_binding_description()
    {
        static_assert(Attribute_Count< VERTEX_TYPE >::value > 0, "Unknown vertex type");

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(VERTEX_TYPE);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    // =========================================================
    // Get_attribute_descriptions
    // =========================================================

    namespace Detail
    {
        inline VkVertexInputAttributeDescription Make_attribute(uint32_t _location, VkFormat _format, size_t _offset)
        {
            VkVertexInputAttributeDescription attribute{};
            attribute.binding = 0;
            attribute.location = _location;
            attribute.format = _format;
            attribute.offset = static_cast<uint32_t>(_offset);
            return attribute;
        }

        // The five attributes every lit mesh vertex shares, at the locations
        // mesh_vertex_input.glsl declares them at: both sides take them from
        // the GPU_VERTEX_LOCATION_* macros of gpu_shared.h. Written once for
        // both the static and the skinned vertex, which have the same member
        // names:
        //   position (vec3, R32G32B32_SFLOAT)
        //   normal   (vec3, A2B10G10R10_SNORM_PACK32)
        //   tangent  (vec4, A2B10G10R10_SNORM_PACK32: xyz = direction, w = bitangent sign)
        //   uv       (vec2, R32G32_SFLOAT)
        //   color    (vec4, R8G8B8A8_UNORM, per-vertex tint)
        // An attribute is stored at the index of its location, so the array
        // is in location order whatever the values of the macros are.
        template< typename VERTEX_TYPE, size_t COUNT >
        inline void Fill_lit_mesh_attributes(std::array< VkVertexInputAttributeDescription, COUNT >& _attributes)
        {
            static_assert(COUNT >= 5, "A lit mesh vertex has at least five attributes");
            static_assert(GPU_VERTEX_LOCATION_POSITION < 5 && GPU_VERTEX_LOCATION_NORMAL < 5 && GPU_VERTEX_LOCATION_TANGENT < 5
                          && GPU_VERTEX_LOCATION_UV < 5 && GPU_VERTEX_LOCATION_COLOR < 5,
                          "The lit mesh attribute locations must be 0 to 4: the skinned vertex adds locations 5 and 6");

            _attributes[GPU_VERTEX_LOCATION_POSITION] = Make_attribute(GPU_VERTEX_LOCATION_POSITION, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VERTEX_TYPE, position));
            _attributes[GPU_VERTEX_LOCATION_NORMAL] = Make_attribute(GPU_VERTEX_LOCATION_NORMAL, NORMAL_TANGENT_FORMAT, offsetof(VERTEX_TYPE, normal));
            _attributes[GPU_VERTEX_LOCATION_TANGENT] = Make_attribute(GPU_VERTEX_LOCATION_TANGENT, NORMAL_TANGENT_FORMAT, offsetof(VERTEX_TYPE, tangent));
            _attributes[GPU_VERTEX_LOCATION_UV] = Make_attribute(GPU_VERTEX_LOCATION_UV, VK_FORMAT_R32G32_SFLOAT, offsetof(VERTEX_TYPE, uv));
            _attributes[GPU_VERTEX_LOCATION_COLOR] = Make_attribute(GPU_VERTEX_LOCATION_COLOR, VK_FORMAT_R8G8B8A8_UNORM, offsetof(VERTEX_TYPE, color));
        }
    }

    template< typename VERTEX_TYPE >
    std::array< VkVertexInputAttributeDescription, Attribute_Count< VERTEX_TYPE >::value >
        Get_attribute_descriptions() = delete;

    template<>
    inline std::array< VkVertexInputAttributeDescription, Attribute_Count< CoreTypes::Vertex_Simple >::value >
        Get_attribute_descriptions< CoreTypes::Vertex_Simple >()
    {
        std::array< VkVertexInputAttributeDescription, 2 > attributes{};

        // location 0 - position (vec3), location 1 - color (vec3)
        attributes[0] = Detail::Make_attribute(0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CoreTypes::Vertex_Simple, position));
        attributes[1] = Detail::Make_attribute(1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CoreTypes::Vertex_Simple, color));

        return attributes;
    }

    template<>
    inline std::array< VkVertexInputAttributeDescription, Attribute_Count< CoreTypes::Vertex_Static_Mesh >::value >
        Get_attribute_descriptions< CoreTypes::Vertex_Static_Mesh >()
    {
        std::array< VkVertexInputAttributeDescription, 5 > attributes{};

        Detail::Fill_lit_mesh_attributes< CoreTypes::Vertex_Static_Mesh >(attributes);

        return attributes;
    }

    template<>
    inline std::array< VkVertexInputAttributeDescription, Attribute_Count< CoreTypes::Vertex_Skinned_Mesh >::value >
        Get_attribute_descriptions< CoreTypes::Vertex_Skinned_Mesh >()
    {
        std::array< VkVertexInputAttributeDescription, 7 > attributes{};

        Detail::Fill_lit_mesh_attributes< CoreTypes::Vertex_Skinned_Mesh >(attributes);

        // location 5 - bone_indices (uvec4 in the shader: an integer format needs an integer input)
        attributes[5] = Detail::Make_attribute(5, VK_FORMAT_R16G16B16A16_UINT, offsetof(CoreTypes::Vertex_Skinned_Mesh, bone_indices));

        // location 6 - bone_weights (vec4, sums to exactly 1.0)
        attributes[6] = Detail::Make_attribute(6, VK_FORMAT_R8G8B8A8_UNORM, offsetof(CoreTypes::Vertex_Skinned_Mesh, bone_weights));

        return attributes;
    }

}