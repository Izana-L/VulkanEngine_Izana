#include <Resource_Manager.hpp>

#include <iostream>
#include <stdexcept>

namespace ResourceManager
{

    // =========================================================
    // Key helpers
    // =========================================================

    std::string Resource_Manager::Make_image_key(const std::string& _path, CoreTypes::Pixel_Format _format)
    {
        // The separator cannot appear in a path, so distinct (path, format)
        // pairs always produce distinct keys.
        return _path + '\n' + std::to_string(static_cast<unsigned>(_format));
    }

    // =========================================================
    // Load_mesh: file, deduplicated
    // =========================================================

    std::vector<CoreTypes::Asset_Handle> Resource_Manager::Load_mesh(const std::string& _path)
    {
        auto it = file_mesh_cache.find(_path);
        if (it != file_mesh_cache.end())
        {
            std::cout << "[Resource_Manager] Mesh cache hit: " << _path << "\n";
            return it->second;
        }

        // Cache miss: load all primitives.
        std::vector<CoreTypes::MeshData> loaded = Mesh_Loader::Load(_path);

        std::vector<CoreTypes::Asset_Handle> handles;
        handles.reserve(loaded.size());

        for (CoreTypes::MeshData& mesh_data : loaded)
        {
            Mesh_Optimizer::Log_stats(Mesh_Optimizer::Optimize(mesh_data), _path);
            handles.push_back(meshes.Register(std::move(mesh_data), _path));
        }

        file_mesh_cache[_path] = handles;

        std::cout << "[Resource_Manager] Loaded and cached " << handles.size()
            << " primitive(s) from " << _path << "\n";

        return handles;
    }

    // =========================================================
    // Create_primitive: generated, deduplicated
    // =========================================================
    namespace
    {
        // Readable source string for a primitive entry, e.g.
        // "primitive:sphere(16,8)". Stored on the Mesh_Entry and printed
        // in the cache logs: when two entries look like duplicates, the
        // descriptor is what tells you why.
        std::string Describe(const Primitive_Desc& _desc)
        {
            std::string source = "primitive:";
            source += Type_name(_desc.type);

            const uint8_t used = Spec_of(_desc.type).used_count;
            if (used == 0)
                return source;

            source += "(" + std::to_string(_desc.param1);
            if (used > 1) source += "," + std::to_string(_desc.param2);
            if (used > 2) source += "," + std::to_string(_desc.param3);
            source += ")";

            return source;
        }
    }

    CoreTypes::Asset_Handle Resource_Manager::Create_primitive(const Primitive_Desc& _desc)
    {
        // Descriptors that build the same mesh must reach the cache as the
        // same descriptor, or the same geometry gets stored twice under two
        // keys ({Cube, 1, 999} and {Cube, 23, 214} are both just a cube).
        // Canonicalizing keeps the cache correct in every build; the
        // Validate() report tells the caller what was repaired.
        if (!_desc.Is_canonical())
        {
            std::cout << "[Resource_Manager] Create_primitive: non-canonical descriptor ("
                << Error_message(_desc.Validate()) << "), using its canonical form.\n";
        }

        const Primitive_Desc desc = _desc.Canonical();
        const std::string    source = Describe(desc);

        if (!Is_known_type(desc.type))
            throw std::invalid_argument("Resource_Manager::Create_primitive: unknown primitive type");

        const uint64_t key = desc.To_key();

        auto it = primitive_cache.find(key);
        if (it != primitive_cache.end())
        {
            std::cout << "[Resource_Manager] Primitive cache hit (" << source << ")\n";
            return it->second;
        }

        // Cache miss: generate.
        CoreTypes::MeshData mesh_data = Primitive_Builder::Build(desc);

        Mesh_Optimizer::Log_stats(Mesh_Optimizer::Optimize(mesh_data), source);

        const CoreTypes::Asset_Handle handle = meshes.Register(std::move(mesh_data), source);

        primitive_cache[key] = handle;

        std::cout << "[Resource_Manager] Generated and cached primitive (" << source << ")\n";

        return handle;
    }

    // =========================================================
    // Mesh shared queries
    // =========================================================

    void Resource_Manager::Register_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id)
    {
        meshes.Register_gpu_id(_handle, _gpu_id, "Register_gpu_id");
    }

    uint32_t Resource_Manager::Get_gpu_id(CoreTypes::Asset_Handle _handle) const
    {
        return meshes.Get_gpu_id(_handle);
    }

    bool Resource_Manager::Is_mesh_handle_valid(CoreTypes::Asset_Handle _handle) const
    {
        return meshes.Is_valid(_handle);
    }

    const CoreTypes::MeshData& Resource_Manager::Get_mesh_data(CoreTypes::Asset_Handle _handle) const
    {
        return meshes.Get_data(_handle, "Get_mesh_data");
    }

    // =========================================================
    // Image
    // =========================================================

    CoreTypes::Asset_Handle Resource_Manager::Load_image(const std::string& _path, CoreTypes::Pixel_Format _format)
    {
        const std::string key = Make_image_key(_path, _format);

        auto it = image_cache.find(key);
        if (it != image_cache.end())
        {
            std::cout << "[Resource_Manager] Image cache hit: " << _path << "\n";
            return it->second;
        }

        CoreTypes::ImageData image_data = Image_Loader::Load(_path, _format);

        const CoreTypes::Asset_Handle handle = images.Register(std::move(image_data), _path);

        image_cache[key] = handle;

        std::cout << "[Resource_Manager] Loaded and cached image " << _path << "\n";

        return handle;
    }

    void Resource_Manager::Register_image_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id)
    {
        images.Register_gpu_id(_handle, _gpu_id, "Register_image_gpu_id");
    }

    uint32_t Resource_Manager::Get_image_gpu_id(CoreTypes::Asset_Handle _handle) const
    {
        return images.Get_gpu_id(_handle);
    }

    bool Resource_Manager::Is_image_handle_valid(CoreTypes::Asset_Handle _handle) const
    {
        return images.Is_valid(_handle);
    }

    const CoreTypes::ImageData& Resource_Manager::Get_image_data(CoreTypes::Asset_Handle _handle) const
    {
        return images.Get_data(_handle, "Get_image_data");
    }

    CoreTypes::Asset_Handle Resource_Manager::Register_external_image(const std::string& _name, uint32_t _gpu_id)
    {
        // Validated before any entry is created, so a rejected call leaves
        // no image without a gpu id behind.
        if (_gpu_id == INVALID_GPU_ID)
            throw std::invalid_argument("Resource_Manager::Register_external_image: INVALID_GPU_ID is not a valid gpu id");

        // Empty ImageData: the pixels exist only on the GPU. The source
        // carries a prefix so logs and errors never mistake it for a path.
        const CoreTypes::Asset_Handle handle = images.Register(CoreTypes::ImageData{}, "external:" + _name);

        Register_image_gpu_id(handle, _gpu_id);

        std::cout << "[Resource_Manager] External image '" << _name << "' registered as image id "
            << handle.id << " (gpu_id " << _gpu_id << ")\n";

        return handle;
    }

} // namespace ResourceManager
