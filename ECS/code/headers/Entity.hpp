#pragma once

#include <cstdint>
#include <Id.hpp>
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
    // Sentinel value representing an entity that doesn't exist or
    // hasn't been assigned yet. Built from CoreTypes::INVALID_ID so the
    // sentinel has a single source of truth: its index can never be
    // allocated, so it never collides with a live entity.
    constexpr Entity INVALID_ENTITY = Make_entity(CoreTypes::INVALID_ID, CoreTypes::INVALID_ID);

    // Returns true if the entity is a valid (non-sentinel) value.
    //
    // The sentinel is defined by its INDEX, whatever the generation: no slot
    // with index INVALID_ID is ever allocated, so any such value is
    // "no entity". Comparing against INVALID_ENTITY alone would accept
    // Make_entity(INVALID_ID, 5) and let a stray value reach
    // Sparse_Array::Set() with a slot of ~4 billion.
    constexpr bool Is_valid_entity(Entity _entity)
    {
        return Entity_index(_entity) != CoreTypes::INVALID_ID;
    }

    // Returns true if the entity is the sentinel/invalid value.
    constexpr bool Is_invalid_entity(Entity _entity)
    {
        return _entity == INVALID_ENTITY;
    }

    static_assert(!Is_valid_entity(INVALID_ENTITY));
    static_assert(!Is_valid_entity(Make_entity(CoreTypes::INVALID_ID, 0u)), "the sentinel is the index, not the exact 64-bit value");
    static_assert(Is_valid_entity(Make_entity(0u, CoreTypes::INVALID_ID)), "a valid slot may carry any generation");
    static_assert(Entity_index(Make_entity(7u, 3u)) == 7u);
    static_assert(Entity_generation(Make_entity(7u, 3u)) == 3u);
    static_assert(Make_entity(7u, 3u) != Make_entity(7u, 4u),"Two generations of the same slot must be distinct entities");
    static_assert(Entity_index(INVALID_ENTITY) == CoreTypes::INVALID_ID,"INVALID_ENTITY must carry CoreTypes::INVALID_ID as its index");

}
