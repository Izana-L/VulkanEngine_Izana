#pragma once

#include <Entity.hpp>
#include <Id_Provider.hpp>
#include <IComponent_Storage.hpp>
#include <Component_Storage.hpp>

#include <array>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <memory>
#include <atomic>
#include <algorithm>

namespace ECS
{

    // World: the central ECS container. Owns all entities, component
    // storages, and entity masks. Provides the API to create/destroy
    // entities, add/remove/get components, and query entities by
    // component combination.
    //
    // Memory layout:
    // - entity_masks and alive_flags live on the HEAP (via unique_ptr)
    //   to avoid stack overflow: at MAX_ENTITIES=100000, entity_masks
    //   alone would be 1.6MB, exceeding the default 1MB stack limit.
    // - storages live as a fixed array of unique_ptrs, created lazily
    //   on first use of each component type.
    //
    // Component type IDs are assigned globally (shared across all World
    // instances) via a static atomic counter - Position is always type 0,
    // Velocity always type 1, etc., regardless of which World is used.
    //
    // Query iterates the FIRST component type in the parameter pack.
    // Put the most restrictive/smallest type first for best performance.
    class World
    {
    public:

        static constexpr size_t MAX_ENTITIES = 100000;
        static constexpr size_t MAX_COMPONENT_TYPES = 128;

        using Entity_Mask = std::bitset< MAX_COMPONENT_TYPES >;
    private:
        // =========================================================
        // Members
        // =========================================================

        CoreTypes::Id_Provider id_provider;

        // One storage per component type, lazily created on first use.
        // Stored as unique_ptr<IComponent_Storage> for type erasure -
        // Cast back to Component_Storage<T> in Get_storage<T>().
        std::array< std::unique_ptr< IComponent_Storage >, MAX_COMPONENT_TYPES > storages;

        // Heap-allocated to avoid stack overflow (1.6 MB + 100 KB would
        // exceed the default 1 MB stack limit on most platforms).
        // unique_ptr<array<T,N>> gives the same fixed-size, compile-time-
        // known layout as a plain array, but allocated on the heap.
        std::unique_ptr< std::array< Entity_Mask, MAX_ENTITIES > > entity_masks;
        std::unique_ptr< std::array< bool, MAX_ENTITIES > > alive_flags;

        size_t alive_entity_count;

        // Global counter for component type IDs. inline static so it
        // lives in the header without a separate .cpp definition.
        // atomic for thread safety if types are first used from multiple
        // threads simultaneously (e.g. with a future JobSystem).
        inline static std::atomic< size_t > next_component_id{ 0 };
    public:
        // =========================================================
        // Constructor
        // =========================================================

        World()
            : alive_entity_count(0),
            entity_masks(std::make_unique< std::array< Entity_Mask, MAX_ENTITIES > >()),
            alive_flags(std::make_unique< std::array< bool, MAX_ENTITIES > >())
        {
            // entity_masks: bitsets default-initialize to all zeros
            // alive_flags:  must be explicitly zeroed
            alive_flags->fill(false);
        }

        // =========================================================
        // Entity management
        // =========================================================

        // Creates a new entity with no components and returns its ID.
        // The entity is considered alive from this point even before
        // any components are added.
        Entity Create_entity()
        {
            Entity entity = id_provider.Allocate_id();

            assert(entity < MAX_ENTITIES &&
                "Create_entity() exceeded MAX_ENTITIES - increase MAX_ENTITIES or destroy unused entities");

            Mask_of(entity).reset();
            Alive_flag_of(entity) = true;
            ++alive_entity_count;

            return entity;
        }

        // Destroys an entity and removes all its components from every
        // storage that contains it. The entity ID may be reused by a
        // future Create_entity() call.
        void Destroy_entity(Entity _entity)
        {
            assert(Is_valid_entity(_entity) && "Destroy_entity() called with INVALID_ENTITY");
            assert(Is_alive(_entity) && "Destroy_entity() called on an already destroyed entity");

            // Remove from every storage that contains this entity.
            // Uses type-erased IComponent_Storage so we don't need to
            // know the concrete component types here.
            for (auto& storage : storages)
            {
                if (storage && storage->Has(_entity))
                {
                    storage->Remove(_entity);
                }
            }

            Mask_of(_entity).reset();
            Alive_flag_of(_entity) = false;
            id_provider.Release(_entity);
            --alive_entity_count;
        }

        // Creates a new entity with the same components as _source.
        // Each component is copy-constructed from the source's values.
        // The clone is fully independent - modifying one does not
        // affect the other.
        Entity Clone_entity(Entity _source)
        {
            assert(Is_valid_entity(_source) && "Clone_entity() called with INVALID_ENTITY");
            assert(Is_alive(_source) && "Clone_entity() called on a destroyed entity");

            Entity clone = Create_entity();

            for (size_t i = 0; i < MAX_COMPONENT_TYPES; ++i)
            {
                if (Mask_of(_source).test(i) && storages[i])
                {
                    storages[i]->Clone_to(_source, clone);
                    Mask_of(clone).set(i);
                }
            }

            return clone;
        }

        // Returns true if the entity was created and not yet destroyed.
        bool Is_alive(Entity _entity) const
        {
            assert(Is_valid_entity(_entity) && "Is_alive() called with INVALID_ENTITY");
            assert(_entity < MAX_ENTITIES && "Is_alive() called with out-of-range entity ID");

            return Alive_flag_of(_entity);
        }

        // Returns the number of currently alive entities.
        size_t Entity_count() const
        {
            return alive_entity_count;
        }

        // =========================================================
        // Component management
        // =========================================================

        // Adds a component to an entity, constructing it in place from
        // the given arguments. Returns a reference to the stored component.
        // If the entity already has this component type, it is overwritten.
        template< typename COMPONENT_TYPE, typename... Args >
        COMPONENT_TYPE& Add_component(Entity _entity, Args&&... _args)
        {
            assert(Is_valid_entity(_entity) && "Add_component() called with INVALID_ENTITY");
            assert(Is_alive(_entity) && "Add_component() called on a destroyed entity");
            assert(_entity < MAX_ENTITIES && "Add_component() called with out-of-range entity ID");

            size_t type_id = Component_Id< COMPONENT_TYPE >();

            assert(type_id < MAX_COMPONENT_TYPES &&
                "Add_component() exceeded MAX_COMPONENT_TYPES - increase MAX_COMPONENT_TYPES");

            Mask_of(_entity).set(type_id);

            return Get_or_create_storage< COMPONENT_TYPE >()
                .Emplace(_entity, std::forward< Args >(_args)...);
        }

        // Returns the component if the entity already has it, or creates
        // it with the given arguments if it doesn't. Useful for init
        // patterns where you want to ensure a component exists.
        template< typename COMPONENT_TYPE, typename... Args >
        COMPONENT_TYPE& Get_or_add_component(Entity _entity, Args&&... _args)
        {
            assert(Is_valid_entity(_entity) && "Get_or_add_component() called with INVALID_ENTITY");
            assert(Is_alive(_entity) && "Get_or_add_component() called on a destroyed entity");

            if (Has_component< COMPONENT_TYPE >(_entity))
            {
                return Get_component< COMPONENT_TYPE >(_entity);
            }

            return Add_component< COMPONENT_TYPE >(_entity, std::forward< Args >(_args)...);
        }

        // Removes a component from an entity.
        // No-op if the entity doesn't have this component type.
        template< typename COMPONENT_TYPE >
        void Remove_component(Entity _entity)
        {
            assert(Is_valid_entity(_entity) && "Remove_component() called with INVALID_ENTITY");
            assert(Is_alive(_entity) && "Remove_component() called on a destroyed entity");

            size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (type_id >= MAX_COMPONENT_TYPES)      return;
            if (!Mask_of(_entity).test(type_id))     return;

            Mask_of(_entity).reset(type_id);

            if (storages[type_id])
            {
                storages[type_id]->Remove(_entity);
            }
        }

        // Returns a reference to the component of the given type.
        // Precondition: Has_component<T>(entity) must be true.
        template< typename COMPONENT_TYPE >
        COMPONENT_TYPE& Get_component(Entity _entity)
        {
            assert(Is_valid_entity(_entity) && "Get_component() called with INVALID_ENTITY");
            assert(Is_alive(_entity) && "Get_component() called on a destroyed entity");
            assert(Has_component< COMPONENT_TYPE >(_entity) &&
                "Get_component() called for an entity without this component");

            return Get_storage< COMPONENT_TYPE >().Get(_entity);
        }

        template< typename COMPONENT_TYPE >
        const COMPONENT_TYPE& Get_component(Entity _entity) const
        {
            assert(Is_valid_entity(_entity) && "Get_component() const called with INVALID_ENTITY");
            assert(Has_component< COMPONENT_TYPE >(_entity) &&
                "Get_component() const called for an entity without this component");

            return Get_storage< COMPONENT_TYPE >().Get(_entity);
        }

        // Returns a pointer to the component if the entity has it,
        // or nullptr if it doesn't. Safe alternative to Get_component
        // when you're not sure whether the entity has the component.
        template< typename COMPONENT_TYPE >
        COMPONENT_TYPE* Try_get_component(Entity _entity)
        {
            assert(Is_valid_entity(_entity) && "Try_get_component() called with INVALID_ENTITY");

            if (!Has_component< COMPONENT_TYPE >(_entity)) return nullptr;

            return &Get_storage< COMPONENT_TYPE >().Get(_entity);
        }

        template< typename COMPONENT_TYPE >
        const COMPONENT_TYPE* Try_get_component(Entity _entity) const
        {
            assert(Is_valid_entity(_entity) && "Try_get_component() const called with INVALID_ENTITY");

            if (!Has_component< COMPONENT_TYPE >(_entity)) return nullptr;

            return &Get_storage< COMPONENT_TYPE >().Get(_entity);
        }

        // Returns true if the entity has a component of the given type.
        template< typename COMPONENT_TYPE >
        bool Has_component(Entity _entity) const
        {
            assert(Is_valid_entity(_entity) && "Has_component() called with INVALID_ENTITY");

            size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (type_id >= MAX_COMPONENT_TYPES) return false;

            return Mask_of(_entity).test(type_id);
        }

        // Returns how many entities currently have a component of the given type.
        template< typename COMPONENT_TYPE >
        size_t Entity_count_with() const
        {
            size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (type_id >= MAX_COMPONENT_TYPES || !storages[type_id]) return 0;

            return storages[type_id]->Size();
        }

        // Pre-allocates capacity for a component type to avoid repeated
        // reallocations as entities are added. Call at scene init when
        // you know roughly how many entities of a type to expect.
        template< typename COMPONENT_TYPE >
        void Reserve(size_t _capacity)
        {
            Get_or_create_storage< COMPONENT_TYPE >().Reserve(_capacity);
        }

        // =========================================================
        // Query and iteration
        // =========================================================

        // Calls _function(entity, component_a, component_b, ...) for
        // every entity that has ALL of the listed component types.
        //
        // Iterates the FIRST component type's dense array - put the
        // most restrictive type first for best performance:
        //   world.Query<Rare_Component, Common_Component>(...)
        //
        // WARNING: do NOT add or remove components of the queried types
        // during iteration - this modifies the dense arrays and causes
        // undefined behaviour. Collect entities to modify in a temporary
        // list and process them after the query ends.
        template< typename FIRST_COMPONENT, typename... REST_COMPONENTS, typename FUNCTION >
        void Query(FUNCTION&& _function)
        {
            size_t first_id = Component_Id< FIRST_COMPONENT >();

            if (first_id >= MAX_COMPONENT_TYPES || !storages[first_id]) return;

            // Build required mask: one bit per component type in the query.
            // Any entity whose mask ANDed with required_mask equals
            // required_mask has all the required components.
            Entity_Mask required_mask;
            required_mask.set(first_id);
            (required_mask.set(Component_Id< REST_COMPONENTS >()), ...);

            // Iterate the first type's dense arrays directly - maximally
            // cache-friendly, no indirection for the primary component.
            auto& first_storage = Get_storage< FIRST_COMPONENT >();
            const std::vector< Entity >& entities = first_storage.Get_entities();

            for (size_t i = 0; i < entities.size(); ++i)
            {
                Entity entity = entities[i];

                // O(1) bitmask check before accessing any other storage
                if ((Mask_of(entity) & required_mask) == required_mask)
                {
                    _function(
                        entity,
                        first_storage.Get_components()[i],
                        Get_storage< REST_COMPONENTS >().Get(entity)...
                    );
                }
            }
        }
        template< typename FIRST_COMPONENT, typename... REST_COMPONENTS, typename FUNCTION >
        void Query(FUNCTION&& _function) const
        {
            size_t first_id = Component_Id< FIRST_COMPONENT >();

            if (first_id >= MAX_COMPONENT_TYPES || !storages[first_id]) return;

            Entity_Mask required_mask;
            required_mask.set(first_id);
            (required_mask.set(Component_Id< REST_COMPONENTS >()), ...);

            const auto& first_storage = Get_storage< FIRST_COMPONENT >();  // const version
            const std::vector< Entity >& entities = first_storage.Get_entities();

            for (size_t i = 0; i < entities.size(); ++i)
            {
                Entity entity = entities[i];

                if ((Mask_of(entity) & required_mask) == required_mask)
                {
                    _function(
                        entity,
                        first_storage.Get_components()[i],
                        Get_storage< REST_COMPONENTS >().Get(entity)...
                    );
                }
            }
        }
        // Calls _function(entity, component) for every entity that has
        // the given component type. Simpler than Query when you only
        // need one component type - no mask check needed.
        template< typename COMPONENT_TYPE, typename FUNCTION >
        void Each(FUNCTION&& _function)
        {
            size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (type_id >= MAX_COMPONENT_TYPES || !storages[type_id]) return;

            Get_storage< COMPONENT_TYPE >().Each(std::forward< FUNCTION >(_function));
        }

        template< typename COMPONENT_TYPE, typename FUNCTION >
        void Each(FUNCTION&& _function) const
        {
            size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (type_id >= MAX_COMPONENT_TYPES || !storages[type_id]) return;

            Get_storage< COMPONENT_TYPE >().Each(std::forward< FUNCTION >(_function));
        }

        // =========================================================
        // World management
        // =========================================================

        // Removes all entities and all components. Resets to empty state.
        void Clear()
        {
            for (auto& storage : storages)
            {
                if (storage) storage->Clear();
            }

            entity_masks->fill(Entity_Mask{});
            alive_flags->fill(false);
            alive_entity_count = 0;
        }
       
        // Reorders the component storage of type T so entities appear in
        // _new_order sequence. Both the dense component array and the sparse
        // index map are updated atomically.
        //
        // Preconditions:
        //   - _new_order must be a permutation of all entities that currently
        //     have a component of type T.
        //   - _new_order.size() must equal the number of entities with T.
        //
        // Used by Transform_System to keep Transform_Components in
        // hierarchical order (parents before children) after any Set_parent
        // call, so Transform_System::Update() can run as a single
        // cache-friendly Each() pass without an external sorted list.

        template< typename COMPONENT_TYPE >
        void Reorder_storage(const std::vector<ECS::Entity>& _new_order)
        {
            size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (type_id >= MAX_COMPONENT_TYPES || !storages[type_id]) return;

            Get_storage< COMPONENT_TYPE >().Reorder(_new_order);
        }
    private:

        // =========================================================
        // Access helpers (avoid (*ptr)[i] everywhere)
        // =========================================================

        // Returns a mutable reference to the entity mask of _entity.
        // Encapsulates the unique_ptr dereference so the rest of the
        // code reads as entity_masks[i] rather than (*entity_masks)[i].
        Entity_Mask& Mask_of(Entity _entity)
        {
            return (*entity_masks)[_entity];
        }

        const Entity_Mask& Mask_of(Entity _entity) const
        {
            return (*entity_masks)[_entity];
        }

        // Returns a mutable reference to the alive flag of _entity.
        bool& Alive_flag_of(Entity _entity)
        {
            return (*alive_flags)[_entity];
        }

        bool Alive_flag_of(Entity _entity) const
        {
            return (*alive_flags)[_entity];
        }

        // =========================================================
        // Component type ID system
        // =========================================================

        // Returns a unique, stable ID for each component type T.
        // The static local is initialized exactly once per type, the
        // first time this function is instantiated for that type.
        // atomic fetch_add ensures thread-safe ID assignment even if
        // types are first used from different threads simultaneously.
        template< typename COMPONENT_TYPE >
        static size_t Component_Id()
        {
            static const size_t id = next_component_id.fetch_add(1, std::memory_order_relaxed);

            assert(id < MAX_COMPONENT_TYPES &&
                "Component_Id() exceeded MAX_COMPONENT_TYPES - increase MAX_COMPONENT_TYPES");

            return id;
        }

        // =========================================================
        // Storage management
        // =========================================================

        // Returns the storage for T, creating it if it doesn't exist yet.
        template< typename COMPONENT_TYPE >
        Component_Storage< COMPONENT_TYPE >& Get_or_create_storage()
        {
            size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (!storages[type_id])
            {
                storages[type_id] = std::make_unique< Component_Storage< COMPONENT_TYPE > >();
            }

            return static_cast<Component_Storage< COMPONENT_TYPE >&>(*storages[type_id]);
        }

        // Returns the storage for T.
        // Precondition: the storage must already exist (the type must have
        // been used at least once via Add_component or Reserve).
        template< typename COMPONENT_TYPE >
        Component_Storage< COMPONENT_TYPE >& Get_storage()
        {
            size_t type_id = Component_Id< COMPONENT_TYPE >();

            assert(storages[type_id] &&
                "Get_storage() called for a component type never registered via Add_component");

            return static_cast<Component_Storage< COMPONENT_TYPE >&>(*storages[type_id]);
        }

        template< typename COMPONENT_TYPE >
        const Component_Storage< COMPONENT_TYPE >& Get_storage() const
        {
            size_t type_id = Component_Id< COMPONENT_TYPE >();

            assert(storages[type_id] &&
                "Get_storage() const called for a component type never registered");

            return static_cast<const Component_Storage< COMPONENT_TYPE >&>(*storages[type_id]);
        }

    };

}