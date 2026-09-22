#pragma once

#include <Id.hpp>

#include <cstdint>

namespace CoreTypes
{

    // Asset_Handle: opaque reference to a resource managed by ResourceManager.
    //
    // Combines a numeric ID (slot in the resource table) with a generation
    // counter. The generation is what detects a stale handle: when a slot
    // is released and later reused, the slot's generation advances, and a
    // handle carrying the old generation no longer matches it.
    //
    // Is_valid() is a purely local check ("was this handle ever assigned")
    // and cannot know whether the resource still exists. The generation
    // comparison is performed by ResourceManager on every lookup, in every
    // build configuration: a stale handle is reported as INVALID_GPU_ID by
    // the id queries and rejected with an exception by the data accessors.
    //
    // The Renderer maintains a map asset_handle -> gpu_id internally.
    // EngineCore's extract resolves Asset_Handles to gpu_ids before
    // writing the RenderPacket; the Renderer never sees Asset_Handle.
    struct Asset_Handle
    {
        Id       id = INVALID_ID;
        uint32_t generation = 0;

        // True if this handle was produced by ResourceManager and has not
        // been explicitly reset to INVALID_ASSET_HANDLE. Does NOT check
        // whether the underlying resource still exists; ResourceManager
        // performs that check through the generation on every access.
        bool Is_valid() const
        {
            return CoreTypes::Is_valid(id);
        }

        bool operator==(const Asset_Handle&) const = default;
        bool operator!=(const Asset_Handle&) const = default;
    };

    inline constexpr Asset_Handle INVALID_ASSET_HANDLE{};

} // namespace CoreTypes
