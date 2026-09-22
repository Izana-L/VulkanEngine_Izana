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
    // Register_mesh / Register_image: shared entry creation
    // =========================================================

    CoreTypes::Asset_Handle Resource_Manager::Register_mesh(CoreTypes::MeshData&& _data, const std::string& _source)
    {
        const CoreTypes::Id id = mesh_id_provider.Allocate_id();

        if (id >= static_cast<CoreTypes::Id>(meshes.size()))
            meshes.resize(static_cast<size_t>(id) + 1);

        Mesh_Entry& entry = meshes[id];
        entry.data = std::move(_data);
        entry.gpu_id = INVALID_GPU_ID;
        entry.generation++;
        entry.source = _source;

        CoreTypes::Asset_Handle handle;
        handle.id = id;
        handle.generation = entry.generation;

        return handle;
    }

    CoreTypes::Asset_Handle Resource_Manager::Register_image(CoreTypes::ImageData&& _data, const std::string& _source)
    {
        const CoreTypes::Id id = image_id_provider.Allocate_id();

        if (id >= static_cast<CoreTypes::Id>(images.size()))
            images.resize(static_cast<size_t>(id) + 1);

        Image_Entry& entry = images[id];
        entry.data = std::move(_data);
        entry.gpu_id = INVALID_GPU_ID;
        entry.generation++;
        entry.source = _source;

        CoreTypes::Asset_Handle handle;
        handle.id = id;
        handle.generation = entry.generation;

        return handle;
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
            handles.push_back(Register_mesh(std::move(mesh_data), _path));
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

        const CoreTypes::Asset_Handle handle = Register_mesh(std::move(mesh_data), source);

        primitive_cache[key] = handle;

        std::cout << "[Resource_Manager] Generated and cached primitive (" << source << ")\n";

        return handle;
    }

    // =========================================================
    // Mesh shared queries
    // =========================================================

    void Resource_Manager::Register_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id)
    {
        Mesh_Entry& entry = Get_mesh_entry(_handle, "Register_gpu_id");

        if (_gpu_id == INVALID_GPU_ID)
            throw std::invalid_argument("Resource_Manager::Register_gpu_id: INVALID_GPU_ID is not a valid gpu id");

        if (entry.gpu_id != INVALID_GPU_ID)
        {
            throw std::logic_error(
                "Resource_Manager::Register_gpu_id: mesh " + std::to_string(_handle.id) + " (" + entry.source +
                ") already has gpu id " + std::to_string(entry.gpu_id) +
                "; uploading it again would leak the first GPU buffer");
        }

        entry.gpu_id = _gpu_id;

        std::cout << "[Resource_Manager] gpu_id " << _gpu_id << " registered for mesh id " << _handle.id << "\n";
    }

    uint32_t Resource_Manager::Get_gpu_id(CoreTypes::Asset_Handle _handle) const
    {
        const Mesh_Entry* entry = Find_mesh_entry(_handle);

        return entry ? entry->gpu_id : INVALID_GPU_ID;
    }

    bool Resource_Manager::Is_mesh_handle_valid(CoreTypes::Asset_Handle _handle) const
    {
        return Find_mesh_entry(_handle) != nullptr;
    }

    const CoreTypes::MeshData& Resource_Manager::Get_mesh_data(CoreTypes::Asset_Handle _handle) const
    {
        return Get_mesh_entry(_handle, "Get_mesh_data").data;
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

        const CoreTypes::Asset_Handle handle = Register_image(std::move(image_data), _path);

        image_cache[key] = handle;

        std::cout << "[Resource_Manager] Loaded and cached image " << _path << "\n";

        return handle;
    }

    void Resource_Manager::Register_image_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id)
    {
        Image_Entry& entry = Get_image_entry(_handle, "Register_image_gpu_id");

        if (_gpu_id == INVALID_GPU_ID)
            throw std::invalid_argument("Resource_Manager::Register_image_gpu_id: INVALID_GPU_ID is not a valid gpu id");

        if (entry.gpu_id != INVALID_GPU_ID)
        {
            throw std::logic_error(
                "Resource_Manager::Register_image_gpu_id: image " + std::to_string(_handle.id) + " (" + entry.source +
                ") already has gpu id " + std::to_string(entry.gpu_id));
        }

        entry.gpu_id = _gpu_id;

        std::cout << "[Resource_Manager] gpu_id " << _gpu_id << " registered for image id " << _handle.id << "\n";
    }

    uint32_t Resource_Manager::Get_image_gpu_id(CoreTypes::Asset_Handle _handle) const
    {
        const Image_Entry* entry = Find_image_entry(_handle);

        return entry ? entry->gpu_id : INVALID_GPU_ID;
    }

    bool Resource_Manager::Is_image_handle_valid(CoreTypes::Asset_Handle _handle) const
    {
        return Find_image_entry(_handle) != nullptr;
    }

    const CoreTypes::ImageData& Resource_Manager::Get_image_data(CoreTypes::Asset_Handle _handle) const
    {
        return Get_image_entry(_handle, "Get_image_data").data;
    }

    // =========================================================
    // Internal helpers
    // =========================================================

    const Resource_Manager::Mesh_Entry* Resource_Manager::Find_mesh_entry(CoreTypes::Asset_Handle _handle) const
    {
        if (!_handle.Is_valid()) return nullptr;
        if (_handle.id >= static_cast<CoreTypes::Id>(meshes.size())) return nullptr;

        const Mesh_Entry& entry = meshes[_handle.id];

        return (entry.generation == _handle.generation) ? &entry : nullptr;
    }

    Resource_Manager::Mesh_Entry* Resource_Manager::Find_mesh_entry(CoreTypes::Asset_Handle _handle)
    {
        return const_cast<Mesh_Entry*>(static_cast<const Resource_Manager*>(this)->Find_mesh_entry(_handle));
    }

    const Resource_Manager::Image_Entry* Resource_Manager::Find_image_entry(CoreTypes::Asset_Handle _handle) const
    {
        if (!_handle.Is_valid()) return nullptr;
        if (_handle.id >= static_cast<CoreTypes::Id>(images.size())) return nullptr;

        const Image_Entry& entry = images[_handle.id];

        return (entry.generation == _handle.generation) ? &entry : nullptr;
    }

    Resource_Manager::Image_Entry* Resource_Manager::Find_image_entry(CoreTypes::Asset_Handle _handle)
    {
        return const_cast<Image_Entry*>(static_cast<const Resource_Manager*>(this)->Find_image_entry(_handle));
    }

    const Resource_Manager::Mesh_Entry& Resource_Manager::Get_mesh_entry(CoreTypes::Asset_Handle _handle, const char* _operation) const
    {
        const Mesh_Entry* entry = Find_mesh_entry(_handle);

        if (!entry)
        {
            throw std::invalid_argument(
                std::string("Resource_Manager::") + _operation + ": mesh handle {id=" + std::to_string(_handle.id) +
                ", generation=" + std::to_string(_handle.generation) + "} is invalid or stale");
        }

        return *entry;
    }

    Resource_Manager::Mesh_Entry& Resource_Manager::Get_mesh_entry(CoreTypes::Asset_Handle _handle, const char* _operation)
    {
        return const_cast<Mesh_Entry&>(static_cast<const Resource_Manager*>(this)->Get_mesh_entry(_handle, _operation));
    }

    const Resource_Manager::Image_Entry& Resource_Manager::Get_image_entry(CoreTypes::Asset_Handle _handle, const char* _operation) const
    {
        const Image_Entry* entry = Find_image_entry(_handle);

        if (!entry)
        {
            throw std::invalid_argument(
                std::string("Resource_Manager::") + _operation + ": image handle {id=" + std::to_string(_handle.id) +
                ", generation=" + std::to_string(_handle.generation) + "} is invalid or stale");
        }

        return *entry;
    }

    Resource_Manager::Image_Entry& Resource_Manager::Get_image_entry(CoreTypes::Asset_Handle _handle, const char* _operation)
    {
        return const_cast<Image_Entry&>(static_cast<const Resource_Manager*>(this)->Get_image_entry(_handle, _operation));
    }

} // namespace ResourceManager
