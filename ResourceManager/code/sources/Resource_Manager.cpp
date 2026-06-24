#include <Resource_Manager.hpp>

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace ResourceManager
{

    // =========================================================
    // Mesh
    // =========================================================

    std::vector<CoreTypes::Asset_Handle>
        Resource_Manager::Load_mesh(const std::string& _path)
    {
        std::vector<CoreTypes::MeshData> loaded =
            Mesh_Loader::Load(_path);

        std::vector<CoreTypes::Asset_Handle> handles;
        handles.reserve(loaded.size());

        for (CoreTypes::MeshData& mesh_data : loaded)
        {
            CoreTypes::Id id = mesh_id_provider.Allocate_id();

            // Grow the vector to fit this id if needed.
            if (id >= static_cast<CoreTypes::Id>(meshes.size()))
                meshes.resize(static_cast<size_t>(id) + 1);

            Mesh_Entry& entry = meshes[id];
            entry.data = std::move(mesh_data);
            entry.gpu_id = INVALID_GPU_ID;
            entry.generation++;

            CoreTypes::Asset_Handle handle;
            handle.id = id;
            handle.generation = entry.generation;

            handles.push_back(handle);

            std::cout << "[Resource_Manager] Mesh entry created, id = "
                << id << "\n";
        }

        return handles;
    }

    void Resource_Manager::Register_gpu_id(CoreTypes::Asset_Handle _handle,
        uint32_t                _gpu_id)
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

        std::cout << "[Resource_Manager] gpu_id " << _gpu_id
            << " registered for mesh id " << _handle.id << "\n";
    }

    uint32_t Resource_Manager::Get_gpu_id(CoreTypes::Asset_Handle _handle) const
    {
        const Mesh_Entry& entry = Get_mesh_entry(_handle);

        assert(entry.gpu_id != INVALID_GPU_ID &&
            "Get_gpu_id: gpu_id not registered yet — call Register_gpu_id() "
            "after uploading the mesh to the Renderer");

        return entry.gpu_id;
    }

    const CoreTypes::MeshData&
        Resource_Manager::Get_mesh_data(CoreTypes::Asset_Handle _handle) const
    {
        return Get_mesh_entry(_handle).data;
    }

    // =========================================================
    // Image
    // =========================================================

    CoreTypes::Asset_Handle
        Resource_Manager::Load_image(const std::string& _path,
            CoreTypes::Pixel_Format  _format)
    {
        CoreTypes::ImageData image_data = Image_Loader::Load(_path, _format);

        CoreTypes::Id id = image_id_provider.Allocate_id();

        if (id >= static_cast<CoreTypes::Id>(images.size()))
            images.resize(static_cast<size_t>(id) + 1);

        Image_Entry& entry = images[id];
        entry.data = std::move(image_data);
        entry.generation++;

        CoreTypes::Asset_Handle handle;
        handle.id = id;
        handle.generation = entry.generation;

        std::cout << "[Resource_Manager] Image entry created, id = "
            << id << "\n";

        return handle;
    }

    const CoreTypes::ImageData&
        Resource_Manager::Get_image_data(CoreTypes::Asset_Handle _handle) const
    {
        return Get_image_entry(_handle).data;
    }

    // =========================================================
    // Internal helpers
    // =========================================================

    const Resource_Manager::Mesh_Entry&
        Resource_Manager::Get_mesh_entry(CoreTypes::Asset_Handle _handle) const
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

    const Resource_Manager::Image_Entry&
        Resource_Manager::Get_image_entry(CoreTypes::Asset_Handle _handle) const
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