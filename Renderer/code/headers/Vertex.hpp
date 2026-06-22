#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vector.hpp>


#include <array>
#include <cstdint>


namespace Renderer {

    // =========================================================
    // Vertex_Simple
    // =========================================================

    // Minimal vertex: position + color, no lighting information.
    // Use for: debug geometry (gizmos, wireframes, bounding boxes),
    // simple UI elements, or any geometry that doesn't need lighting.
    struct Vertex_Simple 
    {
        MathLib::Vector3 position;
        MathLib::Vector3 color;

        // Describes how a single vertex is laid out in memory as a whole
        // (its total size and how the GPU should step through an array
        // of these vertices in a vertex buffer).
        static VkVertexInputBindingDescription Get_binding_description() {
            VkVertexInputBindingDescription binding{};
            binding.binding = 0; // index of this binding, matches what's bound at draw time
            binding.stride = sizeof(Vertex_Simple);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX; // advance per-vertex, not per-instance
            return binding;
        }

        // Describes each individual field within the vertex: where it
        // starts (offset), how many components and what type (format),
        // and which "location" in the vertex shader it maps to.
        static std::array<VkVertexInputAttributeDescription, 2> Get_attribute_descriptions() {
            std::array<VkVertexInputAttributeDescription, 2> attributes{};

            attributes[0].binding = 0;
            attributes[0].location = 0; // matches "layout(location = 0)" in the vertex shader
            attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT; // 3 floats = vec3
            attributes[0].offset = offsetof(Vertex_Simple, position);

            attributes[1].binding = 0;
            attributes[1].location = 1;
            attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[1].offset = offsetof(Vertex_Simple, color);

            return attributes;
        }
    };

    // =========================================================
    // Vertex_Static_Mesh
    // =========================================================

    // Full-featured vertex for static (non-animated) geometry, ready for
    // PBR lighting with normal mapping and textures.
    // Use for: regular 3D models, props, environment geometry, anything
    // that doesn't need skeletal animation.
    struct Vertex_Static_Mesh 
    {
        MathLib::Vector3 position;
        MathLib::Vector3 normal;
        MathLib::Vector4 tangent; // .xyz = tangent direction, .w = bitangent sign (+1 or -1)
        MathLib::Vector2 uv;
        MathLib::Vector4 color;   // per-vertex tint, defaults to (1,1,1,1) when unused

        static VkVertexInputBindingDescription Get_binding_description() {
            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex_Static_Mesh);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            return binding;
        }

        static std::array<VkVertexInputAttributeDescription, 5> Get_attribute_descriptions() {
            std::array<VkVertexInputAttributeDescription, 5> attributes{};

            attributes[0].binding = 0;
            attributes[0].location = 0;
            attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3
            attributes[0].offset = offsetof(Vertex_Static_Mesh, position);

            attributes[1].binding = 0;
            attributes[1].location = 1;
            attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3
            attributes[1].offset = offsetof(Vertex_Static_Mesh, normal);

            attributes[2].binding = 0;
            attributes[2].location = 2;
            attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; // vec4 (tangent.xyz + sign in .w)
            attributes[2].offset = offsetof(Vertex_Static_Mesh, tangent);

            attributes[3].binding = 0;
            attributes[3].location = 3;
            attributes[3].format = VK_FORMAT_R32G32_SFLOAT; // vec2
            attributes[3].offset = offsetof(Vertex_Static_Mesh, uv);

            attributes[4].binding = 0;
            attributes[4].location = 4;
            attributes[4].format = VK_FORMAT_R32G32B32A32_SFLOAT; // vec4
            attributes[4].offset = offsetof(Vertex_Static_Mesh, color);

            return attributes;
        }
    };

    // =========================================================
    // Vertex_Skinned_Mesh
    // =========================================================

    // Vertex for skeletal animation (characters, rigged models). Extends
    // Vertex_Static_Mesh's data with bone indices and weights, used by
    // the vertex shader to blend between bone transformations.
    // Not used yet (no animation system exists), but the format is
    // defined now so the pipeline/vertex input layout doesn't need to
    // change later when skeletal animation is implemented.
    struct Vertex_Skinned_Mesh {
        MathLib::Vector3 position;
        MathLib::Vector3 normal;
        MathLib::Vector4 tangent;
        MathLib::Vector2 uv;
        MathLib::Vector4 color;

        // Up to 4 bones can influence a single vertex - a common limit
        // that balances visual quality with shader performance.
        std::array<uint32_t, 4> bone_indices;
        MathLib::Vector4 bone_weights; // should sum to 1.0 across the 4 bones

        static VkVertexInputBindingDescription Get_binding_description() {
            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex_Skinned_Mesh);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            return binding;
        }

        static std::array<VkVertexInputAttributeDescription, 7> Get_attribute_descriptions() {
            std::array<VkVertexInputAttributeDescription, 7> attributes{};

            attributes[0].binding = 0;
            attributes[0].location = 0;
            attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[0].offset = offsetof(Vertex_Skinned_Mesh, position);

            attributes[1].binding = 0;
            attributes[1].location = 1;
            attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[1].offset = offsetof(Vertex_Skinned_Mesh, normal);

            attributes[2].binding = 0;
            attributes[2].location = 2;
            attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            attributes[2].offset = offsetof(Vertex_Skinned_Mesh, tangent);

            attributes[3].binding = 0;
            attributes[3].location = 3;
            attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
            attributes[3].offset = offsetof(Vertex_Skinned_Mesh, uv);

            attributes[4].binding = 0;
            attributes[4].location = 4;
            attributes[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            attributes[4].offset = offsetof(Vertex_Skinned_Mesh, color);

            attributes[5].binding = 0;
            attributes[5].location = 5;
            attributes[5].format = VK_FORMAT_R32G32B32A32_UINT; // 4 unsigned ints
            attributes[5].offset = offsetof(Vertex_Skinned_Mesh, bone_indices);

            attributes[6].binding = 0;
            attributes[6].location = 6;
            attributes[6].format = VK_FORMAT_R32G32B32A32_SFLOAT; // vec4
            attributes[6].offset = offsetof(Vertex_Skinned_Mesh, bone_weights);

            return attributes;
        }
    };

}