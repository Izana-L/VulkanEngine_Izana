#pragma once

#include <Asset_Handle.hpp>
#include <Id_Provider.hpp>

#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ResourceManager
{

    // gpu id of an asset that was never uploaded. Resource_Manager
    // re-exports it as Resource_Manager::INVALID_GPU_ID, which is the name
    // the rest of the engine uses.
    inline constexpr uint32_t INVALID_GPU_ID = std::numeric_limits<uint32_t>::max();

    // Asset_Table<Data>: the generational slot storage of one asset kind.
    //
    // Resource_Manager owns one table per kind (meshes, images). Both kinds
    // follow the same lifecycle: register the CPU data and get a handle,
    // validate the handle on every access, attach the Renderer's gpu id
    // exactly once. That lifecycle lives here, once, instead of being
    // written out for each kind.
    //
    // This is an implementation detail of Resource_Manager: its errors and
    // log lines carry the "Resource_Manager" prefix, and the _operation
    // strings are the names of the public Resource_Manager methods.
    //
    // A handle is current when its generation matches the slot's, so a
    // stale handle is rejected in every build configuration. Entries live
    // in a std::deque, which never relocates its elements when it grows at
    // the back: the references handed out by Get / Get_data stay valid for
    // the lifetime of the table.
    //
    // The special members are the compiler-generated ones, and they are
    // correct: Id_Provider keeps no pointers into itself, so a copy is an
    // independent table and a moved-from table is empty and usable.
    template <typename Data>
    class Asset_Table
    {
    public:

        struct Entry
        {
            Data        data;
            uint32_t    gpu_id = INVALID_GPU_ID;
            std::string source;            // path, primitive description, ...
        };

        // _kind names the asset kind in logs and error messages ("mesh",
        // "image"). It is stored, not copied: pass a string literal.
        explicit Asset_Table(const char* _kind)
            : kind(_kind)
        {
        }

        // Creates a new entry from ready data and returns its handle. The
        // entry starts without a gpu id.
        CoreTypes::Asset_Handle Register(Data&& _data, const std::string& _source)
        {
            const CoreTypes::Id id = id_provider.Allocate_id();

            if (id >= static_cast<CoreTypes::Id>(entries.size()))
                entries.resize(static_cast<size_t>(id) + 1);

            Entry& entry = entries[id];
            entry.data = std::move(_data);
            entry.gpu_id = INVALID_GPU_ID;
            entry.source = _source;

            CoreTypes::Asset_Handle handle;
            handle.id = id;
            handle.generation = id_provider.Generation(id);

            return handle;
        }

        // nullptr for an invalid or stale handle.
        const Entry* Find(CoreTypes::Asset_Handle _handle) const
        {
            if (!id_provider.Is_current(_handle.id, _handle.generation)) return nullptr;
            if (_handle.id >= static_cast<CoreTypes::Id>(entries.size())) return nullptr;

            return &entries[_handle.id];
        }

        Entry* Find(CoreTypes::Asset_Handle _handle)
        {
            return const_cast<Entry*>(static_cast<const Asset_Table*>(this)->Find(_handle));
        }

        // Throwing variant, for accessors that must return a reference.
        // _operation names the public method that failed. Throws
        // std::invalid_argument for an invalid or stale handle.
        const Entry& Get(CoreTypes::Asset_Handle _handle, const char* _operation) const
        {
            const Entry* entry = Find(_handle);

            if (!entry)
            {
                throw std::invalid_argument(
                    std::string("Resource_Manager::") + _operation + ": " + kind + " handle {id=" +
                    std::to_string(_handle.id) + ", generation=" + std::to_string(_handle.generation) +
                    "} is invalid or stale");
            }

            return *entry;
        }

        Entry& Get(CoreTypes::Asset_Handle _handle, const char* _operation)
        {
            return const_cast<Entry&>(static_cast<const Asset_Table*>(this)->Get(_handle, _operation));
        }

        const Data& Get_data(CoreTypes::Asset_Handle _handle, const char* _operation) const
        {
            return Get(_handle, _operation).data;
        }

        // True if the handle addresses a live entry.
        bool Is_valid(CoreTypes::Asset_Handle _handle) const
        {
            return Find(_handle) != nullptr;
        }

        // Stores the Renderer's gpu id. Throws std::invalid_argument for a
        // stale/invalid handle or for INVALID_GPU_ID, and std::logic_error
        // if the entry already has a gpu id: a second upload of the same
        // asset would leak the first GPU resource.
        void Register_gpu_id(CoreTypes::Asset_Handle _handle, uint32_t _gpu_id, const char* _operation)
        {
            Entry& entry = Get(_handle, _operation);

            if (_gpu_id == INVALID_GPU_ID)
            {
                throw std::invalid_argument(
                    std::string("Resource_Manager::") + _operation + ": INVALID_GPU_ID is not a valid gpu id");
            }

            if (entry.gpu_id != INVALID_GPU_ID)
            {
                throw std::logic_error(
                    std::string("Resource_Manager::") + _operation + ": " + kind + " " +
                    std::to_string(_handle.id) + " (" + entry.source + ") already has gpu id " +
                    std::to_string(entry.gpu_id) + "; uploading it again would leak the first GPU resource");
            }

            entry.gpu_id = _gpu_id;

            std::cout << "[Resource_Manager] gpu_id " << _gpu_id << " registered for "
                << kind << " id " << _handle.id << "\n";
        }

        // INVALID_GPU_ID when the asset was never uploaded, or when the
        // handle is invalid or stale.
        uint32_t Get_gpu_id(CoreTypes::Asset_Handle _handle) const
        {
            const Entry* entry = Find(_handle);

            return entry ? entry->gpu_id : INVALID_GPU_ID;
        }

    private:

        const char*            kind;
        CoreTypes::Id_Provider id_provider;
        std::deque<Entry>      entries;
    };

} // namespace ResourceManager
