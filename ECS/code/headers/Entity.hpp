#pragma once

#include <cstdint>

namespace ECS
{

    // Entity: a unique identifier for an object in the world.
    // An entity has no data or behaviour of its own; it is simply a key
    // used to look up components in the Component_Storage instances
    // owned by the World.
    //
    // Layout (64 bits):
    //   [0  - 31] index       slot in the World's tables (recycled)
    //   [32 - 63] generation  bumped every time the slot is destroyed
    //
    // The generation is what makes a stored Entity safe to keep around:
    // after the entity is destroyed and its slot reused by a new one, the
    // old value still carries the old generation, so World::Is_alive()
    // reports it dead instead of silently addressing the newcomer.
    //
    // Entities are never compared or indexed directly by the rest of the
    // engine: the helpers below are the only way to take one apart.
    using Entity = std::uint64_t;

    // Sentinel value representing an entity that doesn't exist or
    // hasn't been assigned yet. Its index (0xFFFFFFFF) can never be
    // allocated, so it never collides with a live entity.
    constexpr Entity INVALID_ENTITY = ~Entity{ 0 };

    constexpr std::uint32_t Entity_index(Entity _entity)
    {
        return static_cast<std::uint32_t>(_entity & 0xFFFFFFFFull);
    }

    constexpr std::uint32_t Entity_generation(Entity _entity)
    {
        return static_cast<std::uint32_t>(_entity >> 32);
    }

    constexpr Entity Make_entity(std::uint32_t _index, std::uint32_t _generation)
    {
        return (static_cast<Entity>(_generation) << 32) | static_cast<Entity>(_index);
    }

    // Returns true if the entity is a valid (non-sentinel) value.
    constexpr bool Is_valid_entity(Entity _entity)
    {
        return _entity != INVALID_ENTITY;
    }

    // Returns true if the entity is the sentinel/invalid value.
    constexpr bool Is_invalid_entity(Entity _entity)
    {
        return _entity == INVALID_ENTITY;
    }

    static_assert(Entity_index(Make_entity(7u, 3u)) == 7u);
    static_assert(Entity_generation(Make_entity(7u, 3u)) == 3u);
    static_assert(Make_entity(7u, 3u) != Make_entity(7u, 4u),
        "Two generations of the same slot must be distinct entities");

}
