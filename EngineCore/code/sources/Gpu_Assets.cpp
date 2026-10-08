#include <Gpu_Assets.hpp>

#include <Renderer.hpp>
#include <Resource_Manager.hpp>
#include <Material_Component.hpp>

#include <iostream>

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

    void Gpu_Assets::Ensure_mesh_uploaded(CoreTypes::Asset_Handle _mesh)
    {
        if (resources.Get_gpu_id(_mesh) != ResourceManager::Resource_Manager::INVALID_GPU_ID)
            return;

        resources.Register_gpu_id(_mesh, renderer.Upload_mesh(resources.Get_mesh_data(_mesh)));
    }

    void Gpu_Assets::Ensure_image_uploaded(CoreTypes::Asset_Handle _image)
    {
        if (resources.Get_image_gpu_id(_image) != ResourceManager::Resource_Manager::INVALID_GPU_ID)
            return;

        resources.Register_image_gpu_id(_image, renderer.Upload_texture(resources.Get_image_data(_image)));
    }

    uint32_t Gpu_Assets::Create_material(ECS::Material_Component& _material)
    {
        // No shortcut for a material that already has a slot: its fields may
        // have changed since (a copy of a registered material with another
        // tint, an albedo uploaded after the first registration), and the
        // slot would silently keep the old values. The Renderer
        // deduplicates by value, so an unchanged material gets its own slot
        // back and a changed one a new slot.
        Renderer_System::Material_Desc desc;
        desc.base_color = _material.base_color_factor;
        desc.sampler = _material.sampler;
        desc.alpha_mode = _material.alpha_mode;
        desc.alpha_cutoff = _material.alpha_cutoff;

        // The tint alpha only has an effect through the alpha mode. A
        // material that is translucent by its tint but opaque by its mode
        // is almost certainly a mistake (the mode used to be inferred from
        // the alpha), so it is said instead of drawing it solid in silence.
        if (_material.alpha_mode == CoreTypes::Alpha_Mode::Opaque && _material.base_color_factor.a < 1.0f)
        {
            std::cerr << "[Gpu_Assets] A material has a base color alpha of " << _material.base_color_factor.a
                << " but the alpha mode Opaque, which ignores it: set Alpha_Mode::Blend to draw it translucent, "
                "or Alpha_Mode::Mask to cut it out.\n";
        }

        // Albedo not assigned: White (the Material_Desc default). Assigned
        // but with no GPU index (never uploaded, or a stale handle): Error,
        // so the mistake shows up magenta instead of silently white.
        if (_material.albedo.Is_valid())
        {
            const uint32_t texture_index = resources.Get_image_gpu_id(_material.albedo);

            desc.albedo_texture_index = texture_index != ResourceManager::Resource_Manager::INVALID_GPU_ID
                ? texture_index : Renderer_System::Default_Texture::Error;
        }

        // Written together and after the registration succeeded, so the
        // component never holds a slot with the alpha mode of another one.
        _material.gpu_material_id = renderer.Register_material(desc);
        _material.gpu_alpha_mode = desc.alpha_mode;

        return _material.gpu_material_id;
    }

} // namespace EngineCore