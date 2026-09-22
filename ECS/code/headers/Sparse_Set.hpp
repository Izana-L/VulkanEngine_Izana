#pragma once

#include <Entity.hpp>
#include <Sparse_Array.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ECS
{

    // Sparse_Set: an unordered set of Entity IDs that supports O(1)
    // insert, remove, and lookup, while keeping all entities packed
    // in a contiguous dense array for cache-friendly iteration.
    //
    // Internally uses two structures:
    // - A Sparse_Array<uint32_t> mapping each entity SLOT (Entity_index)
    //   to its position in the dense array.
    // - A std::vector<Entity> dense array of all entities in the set,
    //   always compact (no gaps), maintained by swapping with the last
    //   element on removal.
    //
    // Entities are generational (see Entity.hpp). The sparse array is
    // indexed by the slot only, so membership is confirmed by comparing
    // the FULL entity value stored in the dense array: an entity from an
    // older generation of the same slot is reported as absent.
    class Sparse_Set
    {
    private:

        Sparse_Array<uint32_t> sparse;
        std::vector<Entity>    dense;

        // Returns the dense position of _entity, or a null pointer when the
        // entity (with its exact generation) is not in the set.
        const uint32_t* Find_index(Entity _entity) const
        {
            if (!Is_valid_entity(_entity)) return nullptr;

            const uint32_t* index = sparse.Find(Entity_index(_entity));
            if (!index) return nullptr;

            return dense[*index] == _entity ? index : nullptr;
        }

    public:

        // =========================================================
        // Modifiers
        // =========================================================

        // Adds _entity to the set. No-op if it is already present.
        //
        // Throws std::logic_error if the entity's slot is occupied by a
        // DIFFERENT generation: that means a component of a destroyed
        // entity was never removed, and overwriting the mapping would
        // leave that stale element orphaned inside the dense array.
        void Insert(Entity _entity)
        {
            if (!Is_valid_entity(_entity))
                throw std::invalid_argument("Sparse_Set::Insert: INVALID_ENTITY");

            if (Has(_entity)) return;

            if (sparse.Has_value(Entity_index(_entity)))
            {
                throw std::logic_error(
                    "Sparse_Set::Insert: slot " + std::to_string(Entity_index(_entity)) +
                    " still holds an entity of a previous generation");
            }

            sparse.Set(Entity_index(_entity), static_cast<uint32_t>(dense.size()));
            dense.push_back(_entity);
        }

        // Removes _entity from the set. No-op if it is not present.
        void Remove(Entity _entity)
        {
            const uint32_t* found = Find_index(_entity);
            if (!found) return;

            const uint32_t removed_index = *found;
            const Entity   last_entity = dense.back();

            if (last_entity != _entity)
            {
                dense[removed_index] = last_entity;
                sparse.Set(Entity_index(last_entity), removed_index);
            }

            dense.pop_back();
            sparse.Unset(Entity_index(_entity));
        }

        void Clear()
        {
            for (Entity entity : dense)
                sparse.Unset(Entity_index(entity));
            dense.clear();
        }

        void Reserve(size_t _capacity)
        {
            dense.reserve(_capacity);
        }

        // Validates that _new_order is a permutation of the current dense
        // array and returns, for each position of _new_order, the CURRENT
        // dense index of that entity. Nothing is modified.
        //
        // Throws std::invalid_argument when the sizes differ, when an
        // entity is not in the set, or when an entity appears twice.
        // Checked in every build: a partial order silently applied by
        // Reorder() would drop components (Component_Storage::Reorder
        // moves exactly the entries listed) and leave the sparse indices
        // of the missing entities pointing at slots owned by others.
        std::vector<uint32_t> Permutation_indices(const std::vector<Entity>& _new_order) const
        {
            if (_new_order.size() != dense.size())
            {
                throw std::invalid_argument(
                    "Sparse_Set::Reorder: new order has " + std::to_string(_new_order.size()) +
                    " entities but the set holds " + std::to_string(dense.size()));
            }

            std::vector<uint32_t> old_indices;
            old_indices.reserve(_new_order.size());

            std::vector<uint8_t> seen(dense.size(), 0);

            for (Entity entity : _new_order)
            {
                const uint32_t* index = Find_index(entity);

                if (!index)
                {
                    throw std::invalid_argument(
                        "Sparse_Set::Reorder: entity " + std::to_string(entity) +
                        " is not in the set");
                }

                if (seen[*index])
                {
                    throw std::invalid_argument(
                        "Sparse_Set::Reorder: entity " + std::to_string(entity) +
                        " appears twice in the new order");
                }

                seen[*index] = 1;
                old_indices.push_back(*index);
            }

            return old_indices;
        }

        // Reorders the dense array to match _new_order and updates the
        // sparse array so every entity still maps to its correct index.
        //
        // _new_order must be a permutation of the current dense array; it
        // is validated first (see Permutation_indices), so on failure the
        // set is left untouched.
        //
        // Called by Component_Storage::Reorder() when Transform_System
        // needs to keep transforms in hierarchical order (parents before
        // children) so Update() can do a single cache-friendly Each() pass.
        void Reorder(const std::vector<Entity>& _new_order)
        {
            (void)Permutation_indices(_new_order);   // validation only

            for (uint32_t new_index = 0;
                new_index < static_cast<uint32_t>(_new_order.size());
                ++new_index)
            {
                const Entity entity = _new_order[new_index];

                dense[new_index] = entity;
                sparse.Set(Entity_index(entity), new_index);
            }
        }

        // =========================================================
        // Queries
        // =========================================================

        bool Has(Entity _entity) const
        {
            return Find_index(_entity) != nullptr;
        }

        size_t Size() const { return dense.size(); }

        bool Is_empty() const { return dense.empty(); }

        // Dense position of _entity. Throws std::out_of_range if absent.
        uint32_t Index_of(Entity _entity) const
        {
            const uint32_t* index = Find_index(_entity);

            if (!index)
            {
                throw std::out_of_range(
                    "Sparse_Set::Index_of: entity " + std::to_string(_entity) +
                    " is not in the set");
            }

            return *index;
        }

        // Dense position of _entity, or nullptr if absent (no exception).
        const uint32_t* Try_index_of(Entity _entity) const
        {
            return Find_index(_entity);
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
