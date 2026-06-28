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

        Sparse_Array<uint32_t> sparse;
        std::vector<Entity>    dense;

    public:

        // =========================================================
        // Modifiers
        // =========================================================

        void Insert(Entity _entity)
        {
            assert(Is_valid_entity(_entity) && "Insert() called with INVALID_ENTITY");

            if (Has(_entity)) return;

            sparse.Set(_entity, static_cast<uint32_t>(dense.size()));
            dense.push_back(_entity);
        }

        void Remove(Entity _entity)
        {
            assert(Is_valid_entity(_entity) && "Remove() called with INVALID_ENTITY");

            if (!Has(_entity)) return;

            uint32_t removed_index = sparse[_entity];
            Entity   last_entity = dense.back();

            if (last_entity != _entity)
            {
                dense[removed_index] = last_entity;
                sparse.Set(last_entity, removed_index);
            }

            dense.pop_back();
            sparse.Unset(_entity);
        }

        void Clear()
        {
            for (Entity entity : dense)
                sparse.Unset(entity);
            dense.clear();
        }

        void Reserve(size_t _capacity)
        {
            dense.reserve(_capacity);
        }

        // Reorders the dense array to match _new_order and updates the
        // sparse array so every entity still maps to its correct index.
        //
        // Preconditions (asserted in debug):
        //   - _new_order.size() == dense.size()
        //   - _new_order is a permutation of the current dense array
        //     (every entity appears exactly once)
        //
        // Called by Component_Storage::Reorder() when Transform_System
        // needs to keep transforms in hierarchical order (parents before
        // children) so Update() can do a single cache-friendly Each() pass.
        void Reorder(const std::vector<Entity>& _new_order)
        {
            assert(_new_order.size() == dense.size() &&"Reorder(): new_order size must match current dense size");

            // Rebuild dense in the new order and update sparse in one pass.
            for (uint32_t new_index = 0;
                new_index < static_cast<uint32_t>(_new_order.size());
                ++new_index)
            {
                Entity entity = _new_order[new_index];

                assert(Has(entity) && "Reorder(): entity in new_order is not in this Sparse_Set");

                dense[new_index] = entity;
                sparse.Set(entity, new_index);
            }
        }

        // =========================================================
        // Queries
        // =========================================================

        bool Has(Entity _entity) const
        {
            assert(Is_valid_entity(_entity) && "Has() called with INVALID_ENTITY");
            return sparse.Has_value(_entity);
        }

        size_t Size() const { return dense.size(); }

        bool Is_empty() const { return dense.empty(); }

        uint32_t Index_of(Entity _entity) const
        {
            assert(Has(_entity) && "Index_of() called for an entity not in the set");
            return sparse[_entity];
        }

        // =========================================================
        // Iteration
        // =========================================================

        auto begin() { return dense.begin(); }
        auto end() { return dense.end(); }
        auto begin() const { return dense.begin(); }
        auto end()   const { return dense.end(); }

        const std::vector<Entity>& Get_dense() const { return dense; }
    };

} // namespace ECS