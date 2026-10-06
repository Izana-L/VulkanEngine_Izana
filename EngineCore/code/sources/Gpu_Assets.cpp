#include <Gpu_Assets.hpp>

#include <Renderer.hpp>
#include <Resource_Manager.hpp>
#include <Material_Component.hpp>

namespace EngineCore
{

    Gpu_Assets::Gpu_Assets(ResourceManager::Resource_Manager& _resources, Renderer_System::Renderer& _renderer)
        : resources(_resources)
        , renderer(_renderer)
    {}

    CoreTypes::Asset_Handle Gpu_Assets::Create_primitive(const ResourceManager::Primitive_Desc& _desc)
    {
        const CoreTypes::Asset_Handle handle = resources.Create_primitive(_desc);

        Ensure_mesh_uploaded(handle);

        return handle;
    }

    std::vector<CoreTypes::Asset_Handle> Gpu_Assets::Load_mesh(const std::string& _path)
    {
        const std::vector<CoreTypes::Asset_Handle> handles = resources.Load_mesh(_path);

        for (const CoreTypes::Asset_Handle handle : handles)
            Ensure_mesh_uploaded(handle);

        return handles;
    }

    CoreTypes::Asset_Handle Gpu_Assets::Load_image(const std::string& _path, CoreTypes::Pixel_Format _format)
    {
        const CoreTypes::Asset_Handle handle = resources.Load_image(_path, _format);

        Ensure_image_uploaded(handle);

        return handle;
    }

    CoreTypes::Asset_Handle Gpu_Assets::Get_procedural_texture()
    {
        if (!procedural_texture.Is_valid())
        {
            procedural_texture = resources.Register_external_image(
                "procedural", renderer.Get_procedural_texture_index());
        }

        return procedural_texture;
    }

    uint32_t Gpu_Assets::Ensure_mesh_uploaded(CoreTypes::Asset_Handle _mesh)
    {
        const uint32_t existing = resources.Get_gpu_id(_mesh);
        if (existing != ResourceManager::Resource_Manager::INVALID_GPU_ID)
            return existing;

        const uint32_t gpu_id = renderer.Upload_mesh(resources.Get_mesh_data(_mesh));
        resources.Register_gpu_id(_mesh, gpu_id);

        return gpu_id;
    }

    uint32_t Gpu_Assets::Ensure_image_uploaded(CoreTypes::Asset_Handle _image)
    {
        const uint32_t existing = resources.Get_image_gpu_id(_image);
        if (existing != ResourceManager::Resource_Manager::INVALID_GPU_ID)
            return existing;

        const uint32_t bindless_index = renderer.Upload_texture(resources.Get_image_data(_image));
        resources.Register_image_gpu_id(_image, bindless_index);

        return bindless_index;
    }

    uint32_t Gpu_Assets::Create_material(ECS::Material_Component& _material)
    {
        if (_material.gpu_material_id != ECS::Material_Component::INVALID_GPU_MATERIAL_ID)
            return _material.gpu_material_id;

        Renderer_System::Material_Desc desc;
        desc.base_color = _material.base_color_factor;
        desc.sampler = _material.sampler;

        // Albedo not assigned: White (the Material_Desc default). Assigned
        // but with no GPU index (never uploaded, or a stale handle): Error,
        // so the mistake shows up magenta instead of silently white.
        if (_material.albedo.Is_valid())
        {
            const uint32_t texture_index = resources.Get_image_gpu_id(_material.albedo);

            desc.albedo_texture_index = texture_index != ResourceManager::Resource_Manager::INVALID_GPU_ID
                ? texture_index : Renderer_System::Default_Texture::Error;
        }

        _material.gpu_material_id = renderer.Register_material(desc);

        return _material.gpu_material_id;
    }

} // namespace EngineCore