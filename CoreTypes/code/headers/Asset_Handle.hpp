#pragma once

#include <Id.hpp>

#include <cstdint>

namespace CoreTypes
{

    // Asset_Handle: opaque reference to a resource managed by ResourceManager.
    //
    // Combines a numeric ID (slot in the resource table) with a generation
    // counter to detect stale handles: if a resource is destroyed and its
    // slot reused, the old handle's generation won't match the new slot's
    // generation, so Is_valid() returns false.
    //
    // The Renderer maintains a map asset_handle → gpu_id internally.
    // EngineCore's extract resolves Asset_Handles to gpu_ids before
    // writing the RenderPacket — the Renderer never sees Asset_Handle.
    struct Asset_Handle
    {
        Id       id = INVALID_ID;
        uint32_t generation = 0;

        // Returns true if this handle was produced by ResourceManager
        // and has not been explicitly invalidated.
        // Does NOT check whether the underlying resource still exists
        // (generation check is needed for that — done by ResourceManager).
        bool Is_valid() const
        {
            return CoreTypes::Is_valid(id);
        }

        bool operator==(const Asset_Handle&) const = default;
        bool operator!=(const Asset_Handle&) const = default;
    };

    inline constexpr Asset_Handle INVALID_ASSET_HANDLE{};

} // namespace CoreTypes