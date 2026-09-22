#pragma once

#include <IComponent_Storage.hpp>
#include <Sparse_Set.hpp>

#include <stdexcept>
#include <utility>
#include <vector>

namespace ECS
{

    template< typename COMPONENT_TYPE >
    class Component_Storage : public IComponent_Storage
    {
    public:

        using Component_Type = COMPONENT_TYPE;

    private:

        Sparse_Set                    sparse_set;
        std::vector< Component_Type > components;

    public:

        // =========================================================
        // IComponent_Storage overrides
        // =========================================================

        bool Has(Entity _entity) const override
        {
            return sparse_set.Has(_entity);
        }

        void Remove(Entity _entity) override
        {
            const uint32_t* found = sparse_set.Try_index_of(_entity);
            if (!found) return;

            const uint32_t removed_index = *found;
            const uint32_t last_index = static_cast<uint32_t>(components.size()) - 1;

            if (removed_index != last_index)
                components[removed_index] = std::move(components[last_index]);

            components.pop_back();
            sparse_set.Remove(_entity);
        }

        void Clear() override
        {
            sparse_set.Clear();
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
            sparse_set.Reserve(_capacity);
            components.reserve(_capacity);
        }

        // =========================================================
        // Component_Storage specific
        // =========================================================

        Component_Type& Add(Entity _entity, const Component_Type& _component)
        {
            if (const uint32_t* index = sparse_set.Try_index_of(_entity))
            {
                Component_Type& existing = components[*index];
                existing = _component;
                return existing;
            }

            sparse_set.Insert(_entity);
            components.push_back(_component);
            return components.back();
        }

        Component_Type& Add(Entity _entity, Component_Type&& _component)
        {
            if (const uint32_t* index = sparse_set.Try_index_of(_entity))
            {
                Component_Type& existing = components[*index];
                existing = std::move(_component);
                return existing;
            }

            sparse_set.Insert(_entity);
            components.push_back(std::move(_component));
            return components.back();
        }

        template< typename... Args >
        Component_Type& Emplace(Entity _entity, Args&&... _args)
        {
            if (const uint32_t* index = sparse_set.Try_index_of(_entity))
            {
                Component_Type& existing = components[*index];
                existing = Component_Type(std::forward< Args >(_args)...);
                return existing;
            }

            sparse_set.Insert(_entity);
            components.emplace_back(std::forward< Args >(_args)...);
            return components.back();
        }

        // Throws std::out_of_range if _entity has no component here.
        Component_Type& Get(Entity _entity)
        {
            return components[sparse_set.Index_of(_entity)];
        }

        const Component_Type& Get(Entity _entity) const
        {
            return components[sparse_set.Index_of(_entity)];
        }

        // Returns nullptr instead of throwing when the component is absent.
        Component_Type* Try_get(Entity _entity)
        {
            const uint32_t* index = sparse_set.Try_index_of(_entity);
            return index ? &components[*index] : nullptr;
        }

        const Component_Type* Try_get(Entity _entity) const
        {
            const uint32_t* index = sparse_set.Try_index_of(_entity);
            return index ? &components[*index] : nullptr;
        }

        bool Is_empty() const { return components.empty(); }

        template< typename FUNCTION >
        void Each(FUNCTION&& _function)
        {
            const auto& entities = sparse_set.Get_dense();
            for (size_t i = 0; i < components.size(); ++i)
                _function(entities[i], components[i]);
        }

        template< typename FUNCTION >
        void Each(FUNCTION&& _function) const
        {
            const auto& entities = sparse_set.Get_dense();
            for (size_t i = 0; i < components.size(); ++i)
                _function(entities[i], components[i]);
        }

        const std::vector< Entity >& Get_entities()   const { return sparse_set.Get_dense(); }
        std::vector< Component_Type >& Get_components() { return components; }
        const std::vector< Component_Type >& Get_components() const { return components; }

        // =========================================================
        // Reorder
        // =========================================================

        // Reorders both the component array and the sparse_set to match
        // _new_order. After this call, iterating with Each() visits
        // entities in _new_order sequence.
        //
        // Used by Transform_System to keep Transform_Components in
        // parent-before-child order so Update() needs only one
        // cache-friendly forward pass, with no external sorted list.
        //
        // _new_order must be a permutation of every entity in this
        // storage. It is validated BEFORE anything is moved (see
        // Sparse_Set::Permutation_indices), so an invalid order throws
        // std::invalid_argument and leaves the storage untouched. The
        // validation runs in every build configuration: a shorter order
        // applied blindly would move only the listed components and
        // destroy the rest when the old array is discarded.
        //
        // Cost: O(n): one pass to validate, one to build the reordered
        // component buffer, one in Sparse_Set::Reorder() to update the
        // sparse indices. Called only when the hierarchy changes.
        void Reorder(const std::vector<Entity>& _new_order)
        {
            const std::vector<uint32_t> old_indices = sparse_set.Permutation_indices(_new_order);

            std::vector< Component_Type > reordered;
            reordered.reserve(old_indices.size());

            for (uint32_t old_index : old_indices)
                reordered.push_back(std::move(components[old_index]));

            components = std::move(reordered);

            sparse_set.Reorder(_new_order);
        }
    };

} // namespace ECS
