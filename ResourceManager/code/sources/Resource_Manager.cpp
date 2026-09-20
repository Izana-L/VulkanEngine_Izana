#include <Resource_Manager.hpp>

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace ResourceManager
{

    // =========================================================
    // Key helpers
    // =========================================================

    uint64_t Resource_Manager::Make_file_key(const std::string& _path)
    {
        // FNV64 of the path, with bit 63 cleared to stay in the
        // file-asset key space (primitives use bit 63 = 1).
        uint64_t key = CoreTypes::fnv64(_path);
        key &= ~(uint64_t(1) << 63);
        return key;
    }

    uint64_t Resource_Manager::Make_image_key(const std::string& _path,
        CoreTypes::Pixel_Format  _format)
    {
        // Fold the format into the path hash so the same file requested
        // with different formats produces distinct keys.
        uint64_t key = CoreTypes::fnv64(_path);
        key ^= (static_cast<uint64_t>(_format) << 8);
        key &= ~(uint64_t(1) << 63);
        return key;
    }

    // =========================================================
    // Register_mesh — shared entry creation
    // =========================================================

    CoreTypes::Asset_Handle Resource_Manager::Register_mesh(CoreTypes::MeshData&& _data, const std::string& _source)
    {
        CoreTypes::Id id = mesh_id_provider.Allocate_id();

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
    // =========================================================
    // Register_image — shared entry creation
    // =========================================================

    CoreTypes::Asset_Handle Resource_Manager::Register_image(CoreTypes::ImageData&& _data, const std::string& _source)
    {
        CoreTypes::Id id = image_id_provider.Allocate_id();

        if (id >= static_cast<CoreTypes::Id>(images.size()))
            images.resize(static_cast<size_t>(id) + 1);

        Image_Entry& entry = images[id];
        entry.data = std::move(_data);
        entry.generation++;
        entry.source = _source;

        CoreTypes::Asset_Handle handle;
        handle.id = id;
        handle.generation = entry.generation;

        return handle;
    }
    // =========================================================
    // Load_mesh — file, deduplicated
    // =========================================================

    std::vector<CoreTypes::Asset_Handle>Resource_Manager::Load_mesh(const std::string& _path)   
    {
        const uint64_t key = Make_file_key(_path);

        // Cache hit — confirm via full path comparison (anti-collision).
        auto it = mesh_cache.find(key);
        if (it != mesh_cache.end())
        {
            const auto& cached = it->second;
            if (!cached.empty() && meshes[cached.front().id].source == _path)
            {
                std::cout << "[Resource_Manager] Mesh cache hit: " << _path << "\n";
                return cached;
            }
            // Hash collision with a different path — fall through and load.
            std::cout << "[Resource_Manager] Hash collision on mesh key, reloading.\n";
        }

        // Cache miss — load all primitives.
        std::vector<CoreTypes::MeshData> loaded = Mesh_Loader::Load(_path);

        std::vector<CoreTypes::Asset_Handle> handles;
        handles.reserve(loaded.size());

        for (CoreTypes::MeshData& mesh_data : loaded)
            handles.push_back(Register_mesh(std::move(mesh_data), _path));

        mesh_cache[key] = handles;

        std::cout << "[Resource_Manager] Loaded and cached " << handles.size()
            << " primitive(s) from " << _path << "\n";

        return handles;
    }

    // =========================================================
    // Create_primitive — generated, deduplicated
    // =========================================================

    CoreTypes::Asset_Handle Resource_Manager::Create_primitive(const Primitive_Desc& _desc)  
    {
        const uint64_t key = _desc.To_key();

        // Primitive keys are collision-free by construction, but we still
        // store them in the same cache map keyed by the structured key.
        auto it = mesh_cache.find(key);
        if (it != mesh_cache.end() && !it->second.empty())
        {
            std::cout << "[Resource_Manager] Primitive cache hit (key " << key << ")\n";
            return it->second.front();
        }

        // Cache miss — generate.
        CoreTypes::MeshData mesh_data = Primitive_Builder::Build(_desc);

        const std::string source = "primitive:" + std::to_string(key);
        CoreTypes::Asset_Handle handle = Register_mesh(std::move(mesh_data), source);

        mesh_cache[key] = { handle };

        std::cout << "[Resource_Manager] Generated and cached primitive (key " << key << ")\n";

        return handle;
    }

    // =========================================================
    // Mesh shared queries
    // =========================================================

    void Resource_Manager::Register_gpu_id(CoreTypes::Asset_Handle _handle,uint32_t _gpu_id)
    {
        assert(CoreTypes::Is_valid(_handle.id) &&
            "Register_gpu_id: invalid handle");
        assert(_handle.id < static_cast<CoreTypes::Id>(meshes.size()) &&
            "Register_gpu_id: handle id out of range");

        Mesh_Entry& entry = meshes[_handle.id];

        assert(entry.generation == _handle.generation &&
            "Register_gpu_id: stale handle (generation mismatch)");
        assert(entry.gpu_id == INVALID_GPU_ID &&
            "Register_gpu_id: gpu_id already registered for this handle");

        entry.gpu_id = _gpu_id;

        std::cout << "[Resource_Manager] gpu_id " << _gpu_id << " registered for mesh id " << _handle.id << "\n";
    }

    uint32_t Resource_Manager::Get_gpu_id(CoreTypes::Asset_Handle _handle) const
    {
        const Mesh_Entry& entry = Get_mesh_entry(_handle);

        return entry.gpu_id;
    }

    const CoreTypes::MeshData& Resource_Manager::Get_mesh_data(CoreTypes::Asset_Handle _handle) const
    {
        return Get_mesh_entry(_handle).data;
    }

    // =========================================================
    // Image
    // =========================================================

    CoreTypes::Asset_Handle Resource_Manager::Load_image(const std::string& _path, CoreTypes::Pixel_Format  _format)   
    {
        const uint64_t key = Make_image_key(_path, _format);

        auto it = image_cache.find(key);
        if (it != image_cache.end() &&
            images[it->second.id].source == _path)
        {
            std::cout << "[Resource_Manager] Image cache hit: " << _path << "\n";
            return it->second;
        }

        CoreTypes::ImageData image_data = Image_Loader::Load(_path, _format);

        CoreTypes::Asset_Handle handle = Register_image(std::move(image_data), _path);

        image_cache[key] = handle;

        std::cout << "[Resource_Manager] Loaded and cached image " << _path << "\n";

        return handle;
    }

    const CoreTypes::ImageData& Resource_Manager::Get_image_data(CoreTypes::Asset_Handle _handle) const     
    {
        return Get_image_entry(_handle).data;
    }

    // =========================================================
    // Internal helpers
    // =========================================================

    const Resource_Manager::Mesh_Entry& Resource_Manager::Get_mesh_entry(CoreTypes::Asset_Handle _handle) const    
    {
        assert(CoreTypes::Is_valid(_handle.id) &&
            "Get_mesh_entry: invalid handle");
        assert(_handle.id < static_cast<CoreTypes::Id>(meshes.size()) &&
            "Get_mesh_entry: handle id out of range");

        const Mesh_Entry& entry = meshes[_handle.id];

        assert(entry.generation == _handle.generation &&
            "Get_mesh_entry: stale handle (generation mismatch)");

        return entry;
    }

    const Resource_Manager::Image_Entry& Resource_Manager::Get_image_entry(CoreTypes::Asset_Handle _handle) const   
    {
        assert(CoreTypes::Is_valid(_handle.id) &&
            "Get_image_entry: invalid handle");
        assert(_handle.id < static_cast<CoreTypes::Id>(images.size()) &&
            "Get_image_entry: handle id out of range");

        const Image_Entry& entry = images[_handle.id];

        assert(entry.generation == _handle.generation &&
            "Get_image_entry: stale handle (generation mismatch)");

        return entry;
    }

} // namespace ResourceManager