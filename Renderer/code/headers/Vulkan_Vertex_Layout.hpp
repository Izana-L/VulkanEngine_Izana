#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vertex.hpp>

#include <array>
#include <cstddef>

namespace Renderer::Vulkan_Vertex_Layout
{

    // Vulkan_Vertex_Layout: translates CoreTypes vertex types into the
    // Vulkan input descriptors that a pipeline needs at creation time.
    //
    // Lives inside the Renderer — CoreTypes::Vertex has no knowledge of
    // Vulkan; this is the only place that bridges the two.
    // Called once per pipeline creation, not per frame.
    //
    // Usage:
    //   auto binding    = Vulkan_Vertex_Layout::Get_binding_description<CoreTypes::Vertex_Static_Mesh>();
    //   auto attributes = Vulkan_Vertex_Layout::Get_attribute_descriptions<CoreTypes::Vertex_Static_Mesh>();

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

    template< typename VERTEX_TYPE >
    VkVertexInputBindingDescription Get_binding_description() = delete;

    template<>
    inline VkVertexInputBindingDescription
        Get_binding_description< CoreTypes::Vertex_Simple >()
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(CoreTypes::Vertex_Simple);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    template<>
    inline VkVertexInputBindingDescription
        Get_binding_description< CoreTypes::Vertex_Static_Mesh >()
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(CoreTypes::Vertex_Static_Mesh);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    template<>
    inline VkVertexInputBindingDescription
        Get_binding_description< CoreTypes::Vertex_Skinned_Mesh >()
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(CoreTypes::Vertex_Skinned_Mesh);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    // =========================================================
    // Get_attribute_descriptions
    // =========================================================

    template< typename VERTEX_TYPE >
    std::array< VkVertexInputAttributeDescription, Attribute_Count< VERTEX_TYPE >::value >
        Get_attribute_descriptions() = delete;

    template<>
    inline std::array< VkVertexInputAttributeDescription, Attribute_Count< CoreTypes::Vertex_Simple >::value >
        Get_attribute_descriptions< CoreTypes::Vertex_Simple >()
    {
        std::array< VkVertexInputAttributeDescription, 2 > attributes{};

        // location 0 — position (vec3)
        attributes[0].binding = 0;
        attributes[0].location = 0;
        attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[0].offset = offsetof(CoreTypes::Vertex_Simple, position);

        // location 1 — color (vec3)
        attributes[1].binding = 0;
        attributes[1].location = 1;
        attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[1].offset = offsetof(CoreTypes::Vertex_Simple, color);

        return attributes;
    }

    template<>
    inline std::array< VkVertexInputAttributeDescription, Attribute_Count< CoreTypes::Vertex_Static_Mesh >::value >
        Get_attribute_descriptions< CoreTypes::Vertex_Static_Mesh >()
    {
        std::array< VkVertexInputAttributeDescription, 5 > attributes{};

        // location 0 — position (vec3)
        attributes[0].binding = 0;
        attributes[0].location = 0;
        attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[0].offset = offsetof(CoreTypes::Vertex_Static_Mesh, position);

        // location 1 — normal (vec3)
        attributes[1].binding = 0;
        attributes[1].location = 1;
        attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[1].offset = offsetof(CoreTypes::Vertex_Static_Mesh, normal);

        // location 2 — tangent (vec4: xyz = direction, w = bitangent sign)
        attributes[2].binding = 0;
        attributes[2].location = 2;
        attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[2].offset = offsetof(CoreTypes::Vertex_Static_Mesh, tangent);

        // location 3 — uv (vec2)
        attributes[3].binding = 0;
        attributes[3].location = 3;
        attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
        attributes[3].offset = offsetof(CoreTypes::Vertex_Static_Mesh, uv);

        // location 4 — color (vec4, per-vertex tint)
        attributes[4].binding = 0;
        attributes[4].location = 4;
        attributes[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[4].offset = offsetof(CoreTypes::Vertex_Static_Mesh, color);

        return attributes;
    }

    template<>
    inline std::array< VkVertexInputAttributeDescription, Attribute_Count< CoreTypes::Vertex_Skinned_Mesh >::value >
        Get_attribute_descriptions< CoreTypes::Vertex_Skinned_Mesh >()
    {
        std::array< VkVertexInputAttributeDescription, 7 > attributes{};

        // location 0 — position (vec3)
        attributes[0].binding = 0;
        attributes[0].location = 0;
        attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[0].offset = offsetof(CoreTypes::Vertex_Skinned_Mesh, position);

        // location 1 — normal (vec3)
        attributes[1].binding = 0;
        attributes[1].location = 1;
        attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[1].offset = offsetof(CoreTypes::Vertex_Skinned_Mesh, normal);

        // location 2 — tangent (vec4)
        attributes[2].binding = 0;
        attributes[2].location = 2;
        attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[2].offset = offsetof(CoreTypes::Vertex_Skinned_Mesh, tangent);

        // location 3 — uv (vec2)
        attributes[3].binding = 0;
        attributes[3].location = 3;
        attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
        attributes[3].offset = offsetof(CoreTypes::Vertex_Skinned_Mesh, uv);

        // location 4 — color (vec4)
        attributes[4].binding = 0;
        attributes[4].location = 4;
        attributes[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[4].offset = offsetof(CoreTypes::Vertex_Skinned_Mesh, color);

        // location 5 — bone_indices (uvec4)
        attributes[5].binding = 0;
        attributes[5].location = 5;
        attributes[5].format = VK_FORMAT_R32G32B32A32_UINT;
        attributes[5].offset = offsetof(CoreTypes::Vertex_Skinned_Mesh, bone_indices);

        // location 6 — bone_weights (vec4, must sum to 1.0)
        attributes[6].binding = 0;
        attributes[6].location = 6;
        attributes[6].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[6].offset = offsetof(CoreTypes::Vertex_Skinned_Mesh, bone_weights);

        return attributes;
    }

} // namespace Renderer::Vulkan_Vertex_Layout