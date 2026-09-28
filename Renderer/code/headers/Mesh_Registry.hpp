#pragma once

#include <Frame_Timeline.hpp>
#include <Geometry_Pool.hpp>
#include <Mesh_GPU.hpp>
#include <MeshData.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Mesh_Registry: the table of uploaded meshes and the lifetime of their
    // geometry.
    //
    // A mesh id (gpu id) is its index in the table, which is also its slot
    // in the GPU mesh table (Mesh_Info_GPU). Ids are handed out in order and
    // never reused: a released mesh keeps its slot, marked as released, so
    // an id held by stale data is skipped instead of drawing another mesh.
    // Since an id identifies one mesh for the whole session, no generation
    // counter is needed.
    //
    // Releasing splits in two. The mesh stops being drawable at once; its
    // geometry range goes back to the allocator only when every frame that
    // may still read it has completed (Frame_Timeline). The registry keeps
    // the queue of ranges waiting for that.
    //
    // The registry does accounting only. Geometry comes from a
    // Geometry_Allocator and completion from a Frame_Timeline, and neither
    // needs a device to be replaced by a stand-in, so the whole class runs
    // without one. Copying the vertices to the GPU is the caller's.
    //
    // Not copyable or movable: the allocator is referenced, not owned, and
    // must outlive the registry. Destroying the registry does not free the
    // ranges: they go back with the allocator.
    class Mesh_Registry
    {
    public:

        // _capacity: number of ids the registry can hand out (the size of
        // the GPU mesh table).
        Mesh_Registry(Geometry_Allocator& _allocator, uint32_t _capacity);

        Mesh_Registry(const Mesh_Registry&) = delete;
        Mesh_Registry& operator=(const Mesh_Registry&) = delete;
        Mesh_Registry(Mesh_Registry&&) = delete;
        Mesh_Registry& operator=(Mesh_Registry&&) = delete;

        // Ids handed out so far, released ones included; the next id.
        uint32_t Get_count() const { return static_cast<uint32_t>(meshes.size()); }

        uint32_t Get_capacity() const { return capacity; }

        uint32_t Get_free_slot_count() const { return capacity - Get_count(); }

        // Released meshes whose range is still waiting for its frames.
        uint32_t Get_retired_count() const { return static_cast<uint32_t>(retired.size()); }

        // True when _id was handed out and has not been released.
        bool Is_drawable(uint32_t _id) const
        {
            return _id < meshes.size() && !meshes[_id].released;
        }

        // The record of _id; _id must have been handed out. A released
        // mesh keeps its record, but its range may already be freed.
        const Mesh_GPU& Get(uint32_t _id) const;

        // Registers _meshes, in order, with consecutive ids, and returns
        // the first one. Each mesh gets its geometry range and its bounding
        // sphere (from the full-precision positions). Every element must
        // be non-null with vertices and indices.
        //
        // Strong guarantee: if the allocator or the memory runs out, the
        // ranges taken so far go back and nothing is registered. Throws
        // std::runtime_error when _meshes.size() exceeds
        // Get_free_slot_count().
        uint32_t Add_batch(const std::vector<const CoreTypes::MeshData*>& _meshes);

        // Undoes Add_batch: frees the ranges and drops the records of every
        // id from _first_id on. For a batch whose upload failed after
        // Add_batch; the ids must not have been released, and no command
        // may read the ranges any more.
        void Discard_batch(uint32_t _first_id);

        // Stops _id from being drawable and queues its range for release
        // after the last frame submitted so far (the tag). Ranges whose
        // frames have completed are freed at once. Returns false, changing
        // nothing, for an id that was never handed out or is already
        // released.
        bool Release(uint32_t _id, const Frame_Timeline& _timeline);

        // Returns to the allocator the ranges of released meshes whose
        // frames have completed.
        void Free_completed(const Frame_Timeline& _timeline);

    private:

        // A released mesh waiting for the frames that may read its range.
        struct Retired_Mesh
        {
            uint32_t id = 0;
            uint64_t last_frame_serial = 0;   // submitted serial at release time
        };

        Geometry_Allocator&       allocator;
        uint32_t                  capacity;

        // Index = gpu id.
        std::vector<Mesh_GPU>     meshes;
        std::vector<Retired_Mesh> retired;
    };

} // namespace Renderer_System
