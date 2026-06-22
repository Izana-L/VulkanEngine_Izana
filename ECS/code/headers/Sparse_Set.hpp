#pragma once

#include <Entity.hpp>
#include <Sparse_Array.hpp>

#include <vector>
#include <cassert>

namespace ECS
{

    // Sparse_Set: an unordered set of Entity IDs that supports O(1)
    // insert, remove, and lookup, while keeping all entities packed
    // in a contiguous dense array for cache-friendly iteration.
    //
    // Internally uses two structures:
    // - A Sparse_Array<uint32_t> mapping each Entity ID to its index
    //   in the dense array (only populated for entities in the set).
    // - A std::vector<Entity> dense array of all entities in the set,
    //   always compact (no gaps), maintained by swapping with the last
    //   element on removal.
    class Sparse_Set
    {
    private:

        // Sparse array: Entity ID → index in the dense array.
        // Only populated for entities currently in the set.
        Sparse_Array<uint32_t> sparse;

        // Dense array: all entities in the set, always compact.
        // Same index as the component data array in Component_Storage.
        std::vector<Entity> dense;
    public:

        // =========================================================
        // Modifiers
        // =========================================================

        // Adds an entity to the set. No-op if already present.
        void Insert(Entity _entity)
        {
            assert(Is_valid_entity(_entity) && "Insert() called with INVALID_ENTITY");

            if (Has(_entity)) return;

            // Store the index where this entity will live in the dense array
            sparse.Set(_entity, static_cast<uint32_t>(dense.size()));
            dense.push_back(_entity);
        }

        // Removes an entity from the set using the swap-with-last trick,
        // keeping the dense array compact without shifting elements.
        // No-op if the entity is not in the set.
        void Remove(Entity _entity)
        {
            assert(Is_valid_entity(_entity) && "Remove() called with INVALID_ENTITY");

            if (!Has(_entity)) return;

            // Index of the entity being removed in the dense array
            uint32_t removed_index = sparse[_entity];

            // The last entity in the dense array - it will take the
            // removed entity's slot to keep the array compact.
            Entity last_entity = dense.back();

            if (last_entity != _entity)
            {
                // Move last entity into the removed slot
                dense[removed_index] = last_entity;

                // Update last entity's sparse entry to its new index
                sparse.Set(last_entity, removed_index);
            }

            // Remove the now-duplicate last element and clear the sparse entry
            dense.pop_back();
            sparse.Unset(_entity);
        }

        // Removes all entities from the set.
        void Clear()
        {
            for (Entity entity : dense)
            {
                sparse.Unset(entity);
            }
            dense.clear();
        }
        // Reserves capacity in the dense entity array to avoid
        // reallocations as entities are inserted.
        void Reserve(size_t _capacity)
        {
            dense.reserve(_capacity);
        }
        // =========================================================
        // Queries
        // =========================================================

        // Returns true if the entity is in the set.
        bool Has(Entity _entity) const
        {
            assert(Is_valid_entity(_entity) && "Has() called with INVALID_ENTITY");
            return sparse.Has_value(_entity);
        }

        // Returns the number of entities currently in the set.
        size_t Size() const
        {
            return dense.size();
        }

        // Returns true if the set contains no entities.
        bool Is_empty() const
        {
            return dense.empty();
        }

        // Returns the dense array index of an entity.
        // Precondition: Has(entity) must be true.
        uint32_t Index_of(Entity _entity) const
        {
            assert(Has(_entity) && "Index_of() called for an entity not in the set");
            return sparse[_entity];
        }

        // =========================================================
        // Iteration (over the dense array, cache-friendly)
        // =========================================================

        // Range-based for support - iterates the dense array directly.
        // WARNING: do NOT insert or remove entities during iteration,
        // as Remove() modifies the dense array in place (swap-with-last),
        // which would invalidate the iteration.
        auto begin() { return dense.begin(); }
        auto end() { return dense.end(); }
        auto begin() const { return dense.begin(); }
        auto end()   const { return dense.end(); }

        // Direct access to the underlying dense array, useful when
        // Component_Storage needs to iterate entities and components
        // in parallel (same index in both arrays).
        const std::vector<Entity>& Get_dense() const
        {
            return dense;
        }

   
    };

}