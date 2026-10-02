#pragma once

#include <IComponent_Storage.hpp>
#include <Sparse_Array.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ECS
{

    // Component_Storage: every component of one type, packed contiguously.
    //
    // Three structures that are always modified together:
    // - entities:   dense array of the entities that own a component.
    // - components: dense array of the components, PARALLEL to `entities`
    //               (components[i] belongs to entities[i]).
    // - sparse:     maps an entity SLOT (Entity_index) to its position in
    //               the two dense arrays.
    //
    // Invariants:
    //   entities.size() == components.size()
    //   for every i:  sparse[Entity_index(entities[i])] == i
    //
    // Entities are generational (see Entity.hpp). `sparse` is indexed by the
    // slot only, so membership is confirmed by comparing the FULL entity
    // value stored in `entities`: an entity from an older generation of the
    // same slot is reported as absent.
    //
    // Lookup, insert and remove are O(1). Removal is swap-and-pop, so the
    // dense arrays never have gaps and iteration stays cache-friendly.
    template< typename COMPONENT_TYPE >
    class Component_Storage : public IComponent_Storage
    {
    public:

        using Component_Type = COMPONENT_TYPE;

    private:

        Sparse_Array< uint32_t >      sparse;
        std::vector< Entity >         entities;
        std::vector< Component_Type > components;

        // =========================================================
        // Internals
        // =========================================================

        // Returns the dense position of _entity, or nullptr when the entity
        // (with its exact generation) has no component here.
        const uint32_t* Find_index(Entity _entity) const
        {
            if (!Is_valid_entity(_entity)) return nullptr;

            const uint32_t* index = sparse.Find(Entity_index(_entity));
            if (!index) return nullptr;

            return entities[*index] == _entity ? index : nullptr;
        }

        // Dense position of _entity. Throws std::out_of_range if absent.
        uint32_t Index_of(Entity _entity) const
        {
            const uint32_t* index = Find_index(_entity);

            if (!index)
            {
                throw std::out_of_range(
                    "Component_Storage::Get: entity " + std::to_string(_entity) +
                    " has no component in this storage");
            }

            return *index;
        }

        // Appends a component for an entity that has none yet.
        //
        // Throws std::invalid_argument for INVALID_ENTITY, and
        // std::logic_error when the slot still holds an entity of a
        // DIFFERENT generation: that means a component of a destroyed entity
        // was never removed, and overwriting the mapping would leave that
        // stale element orphaned in the dense arrays.
        //
        // If anything throws (the component's own constructor, or running
        // out of memory) the three structures are left exactly as they were.
        template< typename... Args >
        Component_Type& Emplace_new(Entity _entity, Args&&... _args)
        {
            if (!Is_valid_entity(_entity))
                throw std::invalid_argument("Component_Storage: INVALID_ENTITY");

            const uint32_t slot        = Entity_index(_entity);
            const uint32_t dense_index = static_cast<uint32_t>(entities.size());

            if (sparse.Has_value(slot))
            {
                throw std::logic_error(
                    "Component_Storage: slot " + std::to_string(slot) +
                    " still holds an entity of a previous generation");
            }

            // 1. May throw bad_alloc (it can allocate a segment). Nothing
            //    else has been touched yet.
            sparse.Set(slot, dense_index);

            try
            {
                // 2. Either of these may throw. Whatever completed is undone
                //    below, so a failure never leaves a half-added entity.
                entities.push_back(_entity);
                components.emplace_back(std::forward< Args >(_args)...);
            }
            catch (...)
            {
                if (entities.size()   > dense_index) entities.pop_back();
                if (components.size() > dense_index) components.pop_back();
                sparse.Unset(slot);
                throw;
            }

            return components.back();
        }

        // Validates that _new_order is a permutation of the entities in this
        // storage and returns, for each position of _new_order, the CURRENT
        // dense index of that entity. Nothing is modified.
        //
        // Throws std::invalid_argument when the sizes differ, when an entity
        // is not in the storage, or when an entity appears twice. Checked in
        // every build: a partial order silently applied would drop components
        // and leave the sparse indices of the missing entities pointing at
        // positions owned by others.
        std::vector< uint32_t > Permutation_indices(const std::vector< Entity >& _new_order) const
        {
            if (_new_order.size() != entities.size())
            {
                throw std::invalid_argument(
                    "Component_Storage::Reorder: new order has " + std::to_string(_new_order.size()) +
                    " entities but the storage holds " + std::to_string(entities.size()));
            }

            std::vector< uint32_t > old_indices;
            old_indices.reserve(_new_order.size());

            std::vector< uint8_t > seen(entities.size(), 0);

            for (Entity entity : _new_order)
            {
                const uint32_t* index = Find_index(entity);

                if (!index)
                {
                    throw std::invalid_argument(
                        "Component_Storage::Reorder: entity " + std::to_string(entity) +
                        " is not in the storage");
                }

                if (seen[*index])
                {
                    throw std::invalid_argument(
                        "Component_Storage::Reorder: entity " + std::to_string(entity) +
                        " appears twice in the new order");
                }

                seen[*index] = 1;
                old_indices.push_back(*index);
            }

            return old_indices;
        }

    public:

        // =========================================================
        // IComponent_Storage overrides
        // =========================================================

        bool Has(Entity _entity) const override
        {
            return Find_index(_entity) != nullptr;
        }

        // Swap-and-pop: the last element takes the removed one's place, in
        // BOTH dense arrays, so they stay parallel and gap-free.
        void Remove(Entity _entity) override
        {
            const uint32_t* found = Find_index(_entity);
            if (!found) return;

            const uint32_t removed_index = *found;   // copy: `found` points into `sparse`
            const uint32_t last_index    = static_cast<uint32_t>(entities.size()) - 1;

            if (removed_index != last_index)
            {
                entities[removed_index]   = entities[last_index];
                components[removed_index] = std::move(components[last_index]);
                sparse.Set(Entity_index(entities[removed_index]), removed_index);
            }

            entities.pop_back();
            components.pop_back();
            sparse.Unset(Entity_index(_entity));
        }

        void Clear() override
        {
            for (Entity entity : entities)
                sparse.Unset(Entity_index(entity));

            entities.clear();
            components.clear();
        }

        size_t Size() const override
        {
            return components.size();
        }

        void Clone_to(Entity _source, Entity _destination) override
        {
            // Get() throws if _source has no component of this type.
            Add(_destination, Get(_source));
        }

        void Reserve(size_t _capacity)
        {
            entities.reserve(_capacity);
            components.reserve(_capacity);
        }

        // =========================================================
        // Component_Storage specific
        // =========================================================

        // Adds a component built from _args, or replaces the existing one.
        // Every Add/Emplace funnels through here, so "exists or not" is
        // decided in a single place.
        template< typename... Args >
        Component_Type& Emplace(Entity _entity, Args&&... _args)
        {
            if (Component_Type* existing = Try_get(_entity))
            {
                *existing = Component_Type(std::forward< Args >(_args)...);
                return *existing;
            }

            return Emplace_new(_entity, std::forward< Args >(_args)...);
        }

        Component_Type& Add(Entity _entity, const Component_Type& _component)
        {
            return Emplace(_entity, _component);
        }

        Component_Type& Add(Entity _entity, Component_Type&& _component)
        {
            return Emplace(_entity, std::move(_component));
        }

        // Throws std::out_of_range if _entity has no component here.
        Component_Type& Get(Entity _entity)
        {
            return components[Index_of(_entity)];
        }

        const Component_Type& Get(Entity _entity) const
        {
            return components[Index_of(_entity)];
        }

        // Returns nullptr instead of throwing when the component is absent.
        Component_Type* Try_get(Entity _entity)
        {
            const uint32_t* index = Find_index(_entity);
            return index ? &components[*index] : nullptr;
        }

        const Component_Type* Try_get(Entity _entity) const
        {
            const uint32_t* index = Find_index(_entity);
            return index ? &components[*index] : nullptr;
        }

        bool Is_empty() const { return components.empty(); }

        template< typename FUNCTION >
        void Each(FUNCTION&& _function)
        {
            for (size_t i = 0; i < components.size(); ++i)
                _function(entities[i], components[i]);
        }

        template< typename FUNCTION >
        void Each(FUNCTION&& _function) const
        {
            for (size_t i = 0; i < components.size(); ++i)
                _function(entities[i], components[i]);
        }

        const std::vector< Entity >& Get_entities()   const { return entities; }
        std::vector< Component_Type >& Get_components() { return components; }
        const std::vector< Component_Type >& Get_components() const { return components; }

        // =========================================================
        // Reorder
        // =========================================================

        // Reorders both dense arrays to match _new_order. After this call,
        // iterating with Each() visits entities in _new_order sequence.
        //
        // Used by Transform_System to keep Transform_Components in
        // parent-before-child order so Update() needs only one
        // cache-friendly forward pass, with no external sorted list.
        //
        // _new_order must be a permutation of every entity in this storage.
        // It is validated BEFORE anything is moved, so an invalid order
        // throws std::invalid_argument and leaves the storage untouched. The
        // validation runs in every build configuration: a shorter order
        // applied blindly would move only the listed components and destroy
        // the rest when the old array is discarded.
        //
        // Cost: O(n): one pass to validate, one to build the reordered
        // component buffer, one to update entities and sparse indices.
        // Called only when the hierarchy changes.
        void Reorder(const std::vector< Entity >& _new_order)
        {
            // 1. Validate once. Throws before anything is modified.
            const std::vector< uint32_t > old_indices = Permutation_indices(_new_order);

            // 2. Build the reordered components. Capacity is reserved up
            //    front, so only a component's move constructor can throw.
            std::vector< Component_Type > reordered;
            reordered.reserve(old_indices.size());

            for (uint32_t old_index : old_indices)
                reordered.push_back(std::move(components[old_index]));

            // 3. Commit. No allocation happens from here on: the vector move
            //    is noexcept and the loop overwrites existing elements.
            components = std::move(reordered);

            for (uint32_t new_index = 0;
                new_index < static_cast<uint32_t>(_new_order.size());
                ++new_index)
            {
                const Entity entity = _new_order[new_index];

                entities[new_index] = entity;
                sparse.Set(Entity_index(entity), new_index);
            }
        }
    };

} // namespace ECS
