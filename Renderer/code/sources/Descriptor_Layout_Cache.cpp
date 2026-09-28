#include <Descriptor_Layout_Cache.hpp>
#include <Vulkan_Utils.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <stdexcept>
#include <string>

namespace Renderer_System
{
    Descriptor_Layout_Cache::Descriptor_Layout_Cache(const Vulkan_Device& _device,
        VkDescriptorSetLayout _bindless_layout)
        : device_handle(_device.Get_logical_device_handle())
    {
        assert(device_handle != VK_NULL_HANDLE);
        assert(_bindless_layout != VK_NULL_HANDLE &&
            "Descriptor_Layout_Cache: el Bindless_Registry debe construirse antes");

        try
        {
            const VkDescriptorSetLayout per_frame = Create_per_frame_layout();
            compute_layouts[Descriptor_Set::Per_Frame] = per_frame;
            graphics_layouts[Descriptor_Set::Per_Frame] = per_frame;

            compute_layouts[Descriptor_Set::Per_Pass] = Create_per_pass_layout();
            graphics_layouts[Descriptor_Set::Per_Pass] = Create_graphics_pass_layout();

            const VkDescriptorSetLayout per_material = Create_per_material_layout();
            compute_layouts[Descriptor_Set::Per_Material] = per_material;
            graphics_layouts[Descriptor_Set::Per_Material] = per_material;

            Check_storage_buffer_budget();
        }
        catch (...)
        {
            // Partially built: release what was created (the destructor
            // does not run for an object whose constructor threw).
            Destroy_owned_layouts();
            throw;
        }

        compute_layouts[Descriptor_Set::Bindless] = _bindless_layout;   // borrowed
        graphics_layouts[Descriptor_Set::Bindless] = _bindless_layout;
    }

    Descriptor_Layout_Cache::~Descriptor_Layout_Cache()
    {
        Destroy_owned_layouts();

        compute_layouts[Descriptor_Set::Bindless] = VK_NULL_HANDLE;
        graphics_layouts[Descriptor_Set::Bindless] = VK_NULL_HANDLE;
    }

    void Descriptor_Layout_Cache::Destroy_owned_layouts()
    {
        // Sets 0 and 2 are shared by both arrays: destroyed once, through
        // the compute array. Set 1 is distinct in each. Set 3 is borrowed.
        const auto destroy = [&](VkDescriptorSetLayout& _layout)
            {
                if (_layout != VK_NULL_HANDLE)
                {
                    vkDestroyDescriptorSetLayout(device_handle, _layout, nullptr);
                    _layout = VK_NULL_HANDLE;
                }
            };

        destroy(compute_layouts[Descriptor_Set::Per_Frame]);
        destroy(compute_layouts[Descriptor_Set::Per_Pass]);
        destroy(compute_layouts[Descriptor_Set::Per_Material]);
        destroy(graphics_layouts[Descriptor_Set::Per_Pass]);

        graphics_layouts[Descriptor_Set::Per_Frame] = VK_NULL_HANDLE;
        graphics_layouts[Descriptor_Set::Per_Material] = VK_NULL_HANDLE;
    }

    VkDescriptorSetLayout Descriptor_Layout_Cache::Get(uint32_t _set, Pipeline_Kind _kind) const
    {
        assert(_set < Descriptor_Set::Count && "indice de conjunto fuera de rango");

        const VkDescriptorSetLayout layout = Data(_kind)[_set];
        assert(layout != VK_NULL_HANDLE);
        return layout;
    }

    const VkDescriptorSetLayout* Descriptor_Layout_Cache::Data(Pipeline_Kind _kind) const
    {
        return (_kind == Pipeline_Kind::Graphics) ? graphics_layouts.data() : compute_layouts.data();
    }

    // ---------- set 0 : por fotograma ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_per_frame_layout()
    {
        std::array<VkDescriptorSetLayoutBinding, 8> bindings{};

       
        bindings[0].binding = Binding_Per_Frame::Frame_UBO;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        
        bindings[1].binding = Binding_Per_Frame::Lights;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        // binding 2 — object buffer: one Object_GPU per draw, indexed by
        // gl_InstanceIndex (the draw's firstInstance). Visible to the vertex
        // stage (model and normal matrices), the fragment stage (material
        // index, flags) and the compute stage (GPU culling reads every
        // object); declaring COMPUTE now keeps this layout stable later.
        bindings[2].binding = Binding_Per_Frame::Objects;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        // bindings 3-4 — cluster grid and compacted light index list:
        // written by cluster_lights.comp, read by mesh.frag.
        bindings[3].binding = Binding_Per_Frame::Cluster_Grid;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[4].binding = Binding_Per_Frame::Cluster_Light_Indices;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        // binding 5 — allocation counter of the light index list. Compute
        // only: the fragment stage reads the ranges, never the counter.
        bindings[5].binding = Binding_Per_Frame::Cluster_Counters;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        // bindings 6-7 — draw commands and draw count written by
        // cull_objects.comp. Compute only: the draw reads them as indirect
        // and count buffers, which is not a descriptor access.
        bindings[6].binding = Binding_Per_Frame::Draw_Commands;
        bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[6].descriptorCount = 1;
        bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[7].binding = Binding_Per_Frame::Draw_Count;
        bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[7].descriptorCount = 1;
        bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        Record_bindings(bindings.data(), static_cast<uint32_t>(bindings.size()));

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create the set 0 layout");
        return layout;
    }

    // ---------- set 1 : per pass, compute ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_per_pass_layout()
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

        // binding 0 — output of procedural.comp. STORAGE_IMAGE, accessed in
        // layout GENERAL between Storage_Image::Begin_write and End_write.
        // Compute stage only: the graphics pipelines read the same image
        // through its bindless slot (set 3), as a sampled image.
        bindings[0].binding = Binding_Per_Pass::Procedural_Output;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        // binding 1 — view space boxes of the clusters, input of
        // cluster_lights.comp. One copy for every frame: rewritten only when
        // the projection changes, behind a barrier against the previous
        // frame's reads.
        bindings[1].binding = Binding_Per_Pass::Cluster_AABBs;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        Record_bindings(bindings.data(), static_cast<uint32_t>(bindings.size()));

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create the set 1 layout");
        return layout;
    }

    // ---------- set 1 : per pass, graphics ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_graphics_pass_layout()
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

        // bindings 0-1 — accumulation and revealage targets of the weighted
        // blended OIT, read with subpassLoad by the composite subpass
        // (oit_composite.frag) in layout SHADER_READ_ONLY_OPTIMAL. An input
        // attachment is only visible to the fragment stage.
        bindings[0].binding = Binding_Graphics_Pass::Oit_Accumulation;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = Binding_Graphics_Pass::Oit_Revealage;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // No storage buffers: nothing to add to the budget.

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create the graphics set 1 layout");
        return layout;
    }

    // ---------- set 2 : per material ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_per_material_layout()
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

        // binding 0 — material table: one Material_GPU per registered
        // material, indexed by Object_GPU::material_index. Visible to the
        // fragment stage (base color, albedo texture and sampler) and the
        // compute stage (passes that need material data, e.g. culling by
        // pass or alpha mode). The vertex stage only forwards the index.
        bindings[0].binding = Binding_Per_Material::Materials;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        // binding 1 — mesh table: one Mesh_Info_GPU per mesh gpu id,
        // indexed by Object_GPU::mesh_index. Visible to the compute stage
        // (the culling pass builds draw commands and tests the bounding
        // volumes) and the vertex stage (bounds.vert draws those volumes).
        bindings[1].binding = Binding_Per_Material::Meshes;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        Record_bindings(bindings.data(), static_cast<uint32_t>(bindings.size()));

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create the set 2 layout");
        return layout;
    }

    // ---------- storage buffer budget ----------
    void Descriptor_Layout_Cache::Record_bindings(const VkDescriptorSetLayoutBinding* _bindings, uint32_t _count)
    {
        for (uint32_t i = 0; i < _count; ++i)
        {
            if (_bindings[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                continue;

            if (_bindings[i].stageFlags & VK_SHADER_STAGE_VERTEX_BIT)   storage_buffers_per_stage[0] += _bindings[i].descriptorCount;
            if (_bindings[i].stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT) storage_buffers_per_stage[1] += _bindings[i].descriptorCount;
            if (_bindings[i].stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)  storage_buffers_per_stage[2] += _bindings[i].descriptorCount;

            storage_buffers_total += _bindings[i].descriptorCount;
        }
    }

    void Descriptor_Layout_Cache::Check_storage_buffer_budget() const
    {
        // Required_Storage_Buffers is what Vulkan_Device demanded from the
        // GPU. Layouts beyond it could exceed the limits of a device that
        // was accepted, so the mismatch is a programming error reported
        // here, not a validation message on some GPUs only. The bindless
        // set holds no storage buffers.
        const uint32_t per_stage = std::max({ storage_buffers_per_stage[0], storage_buffers_per_stage[1], storage_buffers_per_stage[2] });

        if (per_stage > Required_Storage_Buffers || storage_buffers_total > Required_Storage_Buffers)
        {
            throw std::logic_error("Descriptor_Layout_Cache: the layouts use " + std::to_string(per_stage) +
                " storage buffers in one stage and " + std::to_string(storage_buffers_total) +
                " in total, more than Required_Storage_Buffers (" + std::to_string(Required_Storage_Buffers) +
                "): raise it in Descriptor_Sets.hpp");
        }
    }
}