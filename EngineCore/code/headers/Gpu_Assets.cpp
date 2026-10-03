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

    // Create_material, Ensure_mesh_uploaded, Ensure_image_uploaded:
    // moved from Engine.cpp unchanged (see above).

} // namespace EngineCore