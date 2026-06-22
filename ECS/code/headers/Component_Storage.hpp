#pragma once

#include <IComponent_Storage.hpp>
#include <Sparse_Set.hpp>

#include <vector>
#include <cassert>

namespace ECS
{

    template< typename COMPONENT_TYPE >
    class Component_Storage : public IComponent_Storage  // ← hereda ahora
    {
    public:

        using Component_Type = COMPONENT_TYPE;

    private:

        Sparse_Set sparse_set;
        std::vector< Component_Type > components;

    public:

        // =========================================================
        // IComponent_Storage overrides
        // =========================================================

        bool Has(Entity _entity) const override
        {
            assert(Is_valid_entity(_entity) && "Has() called with INVALID_ENTITY");
            return sparse_set.Has(_entity);
        }

        void Remove(Entity _entity) override
        {
            assert(Is_valid_entity(_entity) && "Remove() called with INVALID_ENTITY");

            if (!sparse_set.Has(_entity)) return;

            uint32_t removed_index = sparse_set.Index_of(_entity);
            uint32_t last_index = static_cast<uint32_t>(components.size()) - 1;

            if (removed_index != last_index)
            {
                components[removed_index] = std::move(components[last_index]);
            }

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
            assert(Has(_source) && "Clone_to() called but source entity has no component of this type");

            Add(_destination, Get(_source));
        }

        // Reserves capacity in both the entity and component dense
        // arrays to avoid reallocations as entities are added.
        void Reserve(size_t _capacity)
        {
            sparse_set.Reserve(_capacity);
            components.reserve(_capacity);
        }

        // =========================================================
        // Component_Storage specific (not in IComponent_Storage)
        // =========================================================

        Component_Type& Add(Entity _entity, const Component_Type& _component)
        {
            assert(Is_valid_entity(_entity) && "Add() called with INVALID_ENTITY");

            if (sparse_set.Has(_entity))
            {
                Component_Type& existing = components[sparse_set.Index_of(_entity)];
                existing = _component;
                return existing;
            }

            sparse_set.Insert(_entity);
            components.push_back(_component);

            return components.back();
        }

        Component_Type& Add(Entity _entity, Component_Type&& _component)
        {
            assert(Is_valid_entity(_entity) && "Add() called with INVALID_ENTITY");

            if (sparse_set.Has(_entity))
            {
                Component_Type& existing = components[sparse_set.Index_of(_entity)];
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
            assert(Is_valid_entity(_entity) && "Emplace() called with INVALID_ENTITY");

            if (sparse_set.Has(_entity))
            {
                Component_Type& existing = components[sparse_set.Index_of(_entity)];
                existing = Component_Type(std::forward< Args >(_args)...);
                return existing;
            }

            sparse_set.Insert(_entity);
            components.emplace_back(std::forward< Args >(_args)...);

            return components.back();
        }

        Component_Type& Get(Entity _entity)
        {
            assert(Has(_entity) && "Get() called for an entity without this component");
            return components[sparse_set.Index_of(_entity)];
        }

        const Component_Type& Get(Entity _entity) const
        {
            assert(Has(_entity) && "Get() const called for an entity without this component");
            return components[sparse_set.Index_of(_entity)];
        }

        bool Is_empty() const
        {
            return components.empty();
        }

        template< typename FUNCTION >
        void Each(FUNCTION&& _function)
        {
            const auto& entities = sparse_set.Get_dense();

            for (size_t i = 0; i < components.size(); ++i)
            {
                _function(entities[i], components[i]);
            }
        }

        template< typename FUNCTION >
        void Each(FUNCTION&& _function) const
        {
            const auto& entities = sparse_set.Get_dense();

            for (size_t i = 0; i < components.size(); ++i)
            {
                _function(entities[i], components[i]);
            }
        }

        const std::vector< Entity >& Get_entities() const
        {
            return sparse_set.Get_dense();
        }

        std::vector< Component_Type >& Get_components()
        {
            return components;
        }

        const std::vector< Component_Type >& Get_components() const
        {
            return components;
        }

    
    };

}