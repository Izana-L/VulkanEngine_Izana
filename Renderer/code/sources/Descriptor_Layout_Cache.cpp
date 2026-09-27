#include <Descriptor_Layout_Cache.hpp>
#include <Vulkan_Utils.hpp>

#include <array>
#include <cassert>
#include <stdexcept>

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
            layouts[Descriptor_Set::Per_Frame] = Create_per_frame_layout();
            layouts[Descriptor_Set::Per_Pass] = Create_per_pass_layout();
            layouts[Descriptor_Set::Per_Material] = Create_per_material_layout();
        }
        catch (...)
        {
            // Partially built: release what was created (the destructor
            // does not run for an object whose constructor threw).
            for (uint32_t set = 0; set < Descriptor_Set::Bindless; ++set)
                if (layouts[set] != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(device_handle, layouts[set], nullptr);
            throw;
        }

        layouts[Descriptor_Set::Bindless] = _bindless_layout;   // borrowed
    }

    Descriptor_Layout_Cache::~Descriptor_Layout_Cache()
    {
       
        for (uint32_t set = 0; set < Descriptor_Set::Bindless; ++set)
        {
            if (layouts[set] != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(device_handle, layouts[set], nullptr);
                layouts[set] = VK_NULL_HANDLE;
            }
        }
        layouts[Descriptor_Set::Bindless] = VK_NULL_HANDLE;
    }

    VkDescriptorSetLayout Descriptor_Layout_Cache::Get(uint32_t _set) const
    {
        assert(_set < Descriptor_Set::Count && "indice de conjunto fuera de rango");
        assert(layouts[_set] != VK_NULL_HANDLE);
        return layouts[_set];
    }

    // ---------- set 0 : por fotograma ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_per_frame_layout()
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};

       
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


        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create the set 0 layout");
        return layout;
    }

    // ---------- set 1 : per pass ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_per_pass_layout()
    {
        std::array<VkDescriptorSetLayoutBinding, 1> bindings{};

        // binding 0 — output of procedural.comp. STORAGE_IMAGE, accessed in
        // layout GENERAL between Storage_Image::Begin_write and End_write.
        // Compute stage only: the graphics pipelines read the same image
        // through its bindless slot (set 3), as a sampled image.
        bindings[0].binding = Binding_Per_Pass::Procedural_Output;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create the set 1 layout");
        return layout;
    }

    // ---------- set 2 : per material ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_per_material_layout()
    {
        std::array<VkDescriptorSetLayoutBinding, 1> bindings{};

        // binding 0 — material table: one Material_GPU per registered
        // material, indexed by Object_GPU::material_index. Visible to the
        // fragment stage (base color, albedo texture and sampler) and the
        // compute stage (passes that need material data, e.g. culling by
        // pass or alpha mode). The vertex stage only forwards the index.
        bindings[0].binding = Binding_Per_Material::Materials;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create the set 2 layout");
        return layout;
    }
}