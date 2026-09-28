#include <Mesh_Registry.hpp>

#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace Renderer_System
{

    Mesh_Registry::Mesh_Registry(Geometry_Allocator& _allocator, uint32_t _capacity)
        : allocator(_allocator),
        capacity(_capacity)
    {
        // Ids are indices of a table of fixed size: the storage is taken
        // once, so registering never reallocates the records.
        meshes.reserve(capacity);
        retired.reserve(capacity);
    }

    const Mesh_GPU& Mesh_Registry::Get(uint32_t _id) const
    {
        assert(_id < meshes.size() && "Mesh_Registry::Get: id was never handed out");

        return meshes[_id];
    }

    uint32_t Mesh_Registry::Add_batch(const std::vector<const CoreTypes::MeshData*>& _meshes)
    {
        if (_meshes.size() > Get_free_slot_count())
        {
            throw std::runtime_error("Mesh_Registry: cannot register " + std::to_string(_meshes.size()) + " mesh(es), only " +
                                     std::to_string(Get_free_slot_count()) + " of " + std::to_string(capacity) + " slots are free");
        }

        const uint32_t first_id = Get_count();

        try
        {
            for (const CoreTypes::MeshData* mesh_data : _meshes)
            {
                assert(mesh_data != nullptr && !mesh_data->vertices.empty() && !mesh_data->indices.empty()
                    && "Mesh_Registry::Add_batch: the caller validates every mesh");

                Mesh_GPU mesh;
                mesh.geometry = allocator.Allocate(static_cast<uint32_t>(mesh_data->vertices.size()),
                                                   static_cast<uint32_t>(mesh_data->indices.size()));

                // From the full-precision positions, before packing.
                Mesh_GPU::Compute_bounding_sphere(*mesh_data, mesh.bounds_center, mesh.bounds_radius);

                // Cannot reallocate: the capacity was reserved and the
                // count was checked above. A record whose push_back could
                // fail would leak its range.
                meshes.push_back(mesh);
            }
        }
        catch (...)
        {
            Discard_batch(first_id);
            throw;
        }

        return first_id;
    }

    void Mesh_Registry::Discard_batch(uint32_t _first_id)
    {
        assert(_first_id <= meshes.size() && "Mesh_Registry::Discard_batch: id out of range");

        for (size_t i = _first_id; i < meshes.size(); ++i)
        {
            assert(!meshes[i].released && "Mesh_Registry::Discard_batch: a released mesh is not part of a fresh batch");

            allocator.Free(meshes[i].geometry);
        }

        meshes.erase(meshes.begin() + static_cast<std::ptrdiff_t>(_first_id), meshes.end());
    }

    bool Mesh_Registry::Release(uint32_t _id, const Frame_Timeline& _timeline)
    {
        if (!Is_drawable(_id))
            return false;

        // Frames recorded from now on skip the mesh. Every frame submitted
        // so far may still draw it, so its range waits for the last of them.
        retired.push_back({ _id, _timeline.Get_submitted_serial() });
        meshes[_id].released = true;

        // Freed at once when every submitted frame has already completed.
        Free_completed(_timeline);

        return true;
    }

    void Mesh_Registry::Free_completed(const Frame_Timeline& _timeline)
    {
        for (size_t i = 0; i < retired.size(); )
        {
            if (_timeline.Is_complete(retired[i].last_frame_serial))
            {
                allocator.Free(meshes[retired[i].id].geometry);

                retired[i] = retired.back();
                retired.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }

} // namespace Renderer_System
