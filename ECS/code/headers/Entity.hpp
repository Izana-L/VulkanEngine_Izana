#pragma once

#include <Id.hpp>

namespace ECS
{

    // Entity: a unique identifier for an object in the world.
    // An entity has no data or behaviour of its own - it is simply
    // a numeric ID that acts as a key to look up components in the
    // various Component_Storage instances owned by the World.
    //
    // Using a named alias (rather than a raw Id/uint32_t everywhere)
    // makes intent clear at the call site: a function that takes an
    // Entity is asking for an object in the world, not just any integer.
    using Entity = CoreTypes::Id;

    // Sentinel value representing an entity that doesn't exist or
    // hasn't been assigned yet. Equivalent to INVALID_ID.
    constexpr Entity INVALID_ENTITY = CoreTypes::INVALID_ID;

    // Returns true if the entity is a valid (non-sentinel) value.
    constexpr inline bool Is_valid_entity(Entity _entity)
    {
        return CoreTypes::Is_valid(_entity);
    }

    // Returns true if the entity is the sentinel/invalid value.
    constexpr inline bool Is_invalid_entity(Entity _entity)
    {
        return CoreTypes::Not_valid(_entity);
    }

}