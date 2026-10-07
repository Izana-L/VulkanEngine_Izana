#pragma once

#include <vulkan/vulkan.h>

#include <Descriptor_Sets.hpp>
#include <Vulkan_Descriptor_Utils.hpp>

#include <array>

namespace Renderer_System
{
    // The descriptor set layouts of the Renderer, as data.
    //
    // Each table is the ONE description of a set layout: what
    // Descriptor_Layout_Cache creates the VkDescriptorSetLayout from, what
    // the descriptor pool is sized from (Renderer::Impl::Init_descriptor_pool)
    // and what every Descriptor_Writer takes the descriptor types from. A
    // binding added here is therefore part of the layout, of the pool and of
    // the checked writes at once; the binding numbers are the contract of
    // Descriptor_Sets.hpp (and of the shaders).
    //
    // The storage buffers each stage sees across sets 0-2 are counted from
    // these tables (Descriptor_Layout_Cache), and exceeding
    // Required_Storage_Buffers throws.
    //
    // Set 3, the bindless arrays, has no table here: its sizes are chosen at
    // run time from the device limits (Bindless_Registry).
    namespace Descriptor_Layouts
    {
        using Vulkan_Descriptor_Utils::Layout_Binding;

        // Set 0, shared by the graphics and the compute contract: what one
        // frame slot reads and writes. Bindings 3-7 hold buffers written on
        // the GPU every frame, one copy per slot.
        inline constexpr std::array Per_Frame = {
            // The camera, lights count and cluster parameters of the frame.
            Layout_Binding{ Binding_Per_Frame::Frame_UBO, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },

            // The light array.
            Layout_Binding{ Binding_Per_Frame::Lights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },

            // Object buffer: one Object_GPU per draw, indexed by
            // gl_InstanceIndex (the draw's firstInstance). Visible to the
            // vertex stage (model and normal matrices), the fragment stage
            // (material index, flags) and the compute stage (GPU culling
            // reads every object); declaring COMPUTE now keeps this layout
            // stable later.
            Layout_Binding{ Binding_Per_Frame::Objects, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },

            // Cluster grid and compacted light index list: written by
            // cluster_lights.comp, read by mesh.frag.
            Layout_Binding{ Binding_Per_Frame::Cluster_Grid, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
            Layout_Binding{ Binding_Per_Frame::Cluster_Light_Indices, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },

            // Allocation counter of the light index list. Compute only: the
            // fragment stage reads the ranges, never the counter.
            Layout_Binding{ Binding_Per_Frame::Cluster_Counters, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_COMPUTE_BIT },

            // Draw commands and draw count written by cull_objects.comp.
            // Compute only: the draw reads them as indirect and count
            // buffers, which is not a descriptor access.
            Layout_Binding{ Binding_Per_Frame::Draw_Commands, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_COMPUTE_BIT },
            Layout_Binding{ Binding_Per_Frame::Draw_Count, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_COMPUTE_BIT },
        };

        // Set 1 of the compute contract: the resources the compute passes
        // write, visible to the compute stage only.
        inline constexpr std::array Compute_Per_Pass = {
            // Output of procedural.comp. STORAGE_IMAGE, accessed in layout
            // GENERAL between Storage_Image::Begin_write and End_write. The
            // graphics pipelines read the same image through its bindless
            // slot (set 3), as a sampled image.
            Layout_Binding{ Binding_Per_Pass::Procedural_Output, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                            VK_SHADER_STAGE_COMPUTE_BIT },

            // View space boxes of the clusters, input of cluster_lights.comp.
            // One copy for every frame: rewritten only when the projection
            // changes, behind a barrier against the previous frame's reads.
            Layout_Binding{ Binding_Per_Pass::Cluster_AABBs, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_COMPUTE_BIT },
        };

        // Set 1 of the graphics contract: accumulation and revealage
        // targets of the weighted blended OIT, read with subpassLoad by the
        // composite subpass (oit_composite.frag) in layout
        // SHADER_READ_ONLY_OPTIMAL. An input attachment is only visible to
        // the fragment stage. No storage buffers: nothing to add to the
        // budget.
        inline constexpr std::array Graphics_Per_Pass = {
            Layout_Binding{ Binding_Graphics_Pass::Oit_Accumulation, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                            VK_SHADER_STAGE_FRAGMENT_BIT },
            Layout_Binding{ Binding_Graphics_Pass::Oit_Revealage, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                            VK_SHADER_STAGE_FRAGMENT_BIT },
        };

        // Set 2, shared by both contracts: the global tables.
        inline constexpr std::array Per_Material = {
            // Material table: one Material_GPU per registered material,
            // indexed by Object_GPU::material_index. Visible to the fragment
            // stage (base color, albedo texture and sampler) and the compute
            // stage (passes that need material data, e.g. culling by pass or
            // alpha mode). The vertex stage only forwards the index.
            Layout_Binding{ Binding_Per_Material::Materials, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT },

            // Mesh table: one Mesh_Info_GPU per mesh gpu id, indexed by
            // Object_GPU::mesh_index. Visible to the compute stage (the
            // culling pass builds draw commands and tests the bounding
            // volumes) and the vertex stage (bounds.vert draws those
            // volumes).
            Layout_Binding{ Binding_Per_Material::Meshes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT },
        };
    }
}
