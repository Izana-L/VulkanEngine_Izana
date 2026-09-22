#pragma once

#include <Entity.hpp>
#include <Id_Provider.hpp>
#include <IComponent_Storage.hpp>
#include <Component_Storage.hpp>

#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace ECS
{

    // World: the central ECS container. Owns all entities, component
    // storages, and entity masks. Provides the API to create/destroy
    // entities, add/remove/get components, and query entities by
    // component combination.
    //
    // Entities are generational handles (see Entity.hpp): each slot keeps
    // a generation counter that advances every time the slot is destroyed,
    // so a stale Entity kept by gameplay code is detected by Is_alive()
    // instead of silently addressing whatever entity reused the slot.
    //
    // Memory layout:
    // - entity_masks, alive_flags and generations live on the HEAP (via
    //   unique_ptr) to avoid stack overflow: at MAX_ENTITIES=100000,
    //   entity_masks alone would be 1.6MB, exceeding the default 1MB
    //   stack limit.
    // - storages live as a fixed array of unique_ptrs, created lazily
    //   on first use of each component type.
    //
    // Component type IDs are assigned globally (shared across all World
    // instances) via a static atomic counter - Position is always type 0,
    // Velocity always type 1, etc., regardless of which World is used.
    //
    // Query iterates the FIRST component type in the parameter pack.
    // Put the most restrictive/smallest type first for best performance.
    //
    // Every precondition that protects memory (slot ranges, liveness,
    // component presence) is enforced in every build configuration with
    // an exception; assertions are used only for additional diagnostics.
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
        // cast back to Component_Storage<T> in Get_storage<T>().
        std::array< std::unique_ptr< IComponent_Storage >, MAX_COMPONENT_TYPES > storages;

        // Heap-allocated to avoid stack overflow (see class comment).
        std::unique_ptr< std::array< Entity_Mask, MAX_ENTITIES > > entity_masks;
        std::unique_ptr< std::array< bool, MAX_ENTITIES > >        alive_flags;
        std::unique_ptr< std::array< uint32_t, MAX_ENTITIES > >    generations;

        size_t alive_entity_count = 0;

        // Incremented on every change to the SET of entities or to the SET
        // of components an entity has (create, destroy, clone, add, remove,
        // clear). Systems that cache a derived view of the world (such as
        // Transform_System's hierarchical order) compare it against the
        // value they last saw to know when that view must be rebuilt.
        uint64_t structural_version = 1;

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
            : entity_masks(std::make_unique< std::array< Entity_Mask, MAX_ENTITIES > >()),
            alive_flags(std::make_unique< std::array< bool, MAX_ENTITIES > >()),
            generations(std::make_unique< std::array< uint32_t, MAX_ENTITIES > >())
        {
            // entity_masks: bitsets default-initialize to all zeros.
            // alive_flags / generations: must be explicitly zeroed.
            alive_flags->fill(false);
            generations->fill(0u);
        }

        World(const World&) = delete;
        World& operator=(const World&) = delete;

        // =========================================================
        // Entity management
        // =========================================================

        // Creates a new entity with no components and returns its ID.
        // The entity is considered alive from this point even before
        // any components are added.
        //
        // Throws std::length_error when MAX_ENTITIES are already alive.
        // The slot is handed back to the id provider before throwing, so
        // the world is left exactly as it was.
        Entity Create_entity()
        {
            const CoreTypes::Id index = id_provider.Allocate_id();

            if (static_cast<size_t>(index) >= MAX_ENTITIES)
            {
                id_provider.Release(index);
                throw std::length_error(
                    "World::Create_entity: MAX_ENTITIES (" + std::to_string(MAX_ENTITIES) +
                    ") exceeded - destroy unused entities or raise the limit");
            }

            const Entity entity = Make_entity(index, (*generations)[index]);

            Mask_of(entity).reset();
            Alive_flag_of(entity) = true;
            ++alive_entity_count;
            ++structural_version;

            return entity;
        }

        // Destroys an entity and removes all its components from every
        // storage that contains it. The entity's slot may be reused by a
        // future Create_entity() call, under a new generation, so every
        // copy of the destroyed Entity value is reported dead from now on.
        //
        // Returns false (and does nothing) if the entity is not alive:
        // destroying twice, or destroying a stale handle, is a caller bug
        // that is reported instead of being applied to an unrelated entity.
        bool Destroy_entity(Entity _entity)
        {
            assert(Is_alive(_entity) && "Destroy_entity() called on an entity that is not alive");

            if (!Is_alive(_entity)) return false;

            // The mask is the source of truth for "which storages hold
            // this entity". Removing through it is O(components), and
            // the debug check below verifies the storages agree with it.
            Entity_Mask& mask = Mask_of(_entity);

            for (size_t i = 0; i < MAX_COMPONENT_TYPES; ++i)
            {
                assert((!storages[i] || storages[i]->Has(_entity) == mask.test(i)) &&
                    "World: entity mask and component storages disagree");

                if (mask.test(i) && storages[i])
                {
                    storages[i]->Remove(_entity);
                }
            }

            mask.reset();
            Alive_flag_of(_entity) = false;

            // Advancing the generation is what invalidates every copy of
            // this Entity value still held elsewhere.
            ++(*generations)[Entity_index(_entity)];

            id_provider.Release(Entity_index(_entity));
            --alive_entity_count;
            ++structural_version;

            return true;
        }

        // Creates a new entity with the same components as _source.
        // Each component is copy-constructed from the source's values.
        // The clone is fully independent - modifying one does not
        // affect the other.
        //
        // Throws std::invalid_argument if _source is not alive.
        Entity Clone_entity(Entity _source)
        {
            Require_alive(_source, "Clone_entity");

            const Entity clone = Create_entity();

            for (size_t i = 0; i < MAX_COMPONENT_TYPES; ++i)
            {
                if (Mask_of(_source).test(i) && storages[i])
                {
                    storages[i]->Clone_to(_source, clone);
                    Mask_of(clone).set(i);
                }
            }

            ++structural_version;

            return clone;
        }

        // Returns true if the entity was created and not yet destroyed.
        // Safe to call with any value, including INVALID_ENTITY and stale
        // handles: those simply report false.
        bool Is_alive(Entity _entity) const
        {
            if (!Is_valid_entity(_entity)) return false;

            const uint32_t index = Entity_index(_entity);
            if (static_cast<size_t>(index) >= MAX_ENTITIES) return false;

            return (*alive_flags)[index] &&
                (*generations)[index] == Entity_generation(_entity);
        }

        // Returns the number of currently alive entities.
        size_t Entity_count() const
        {
            return alive_entity_count;
        }

        // See structural_version.
        uint64_t Get_structural_version() const
        {
            return structural_version;
        }

        // =========================================================
        // Component management
        // =========================================================

        // Adds a component to an entity, constructing it in place from
        // the given arguments. Returns a reference to the stored component.
        // If the entity already has this component type, it is overwritten.
        //
        // Throws std::invalid_argument if the entity is not alive.
        template< typename COMPONENT_TYPE, typename... Args >
        COMPONENT_TYPE& Add_component(Entity _entity, Args&&... _args)
        {
            Require_alive(_entity, "Add_component");

            const size_t type_id = Component_Id< COMPONENT_TYPE >();

            Mask_of(_entity).set(type_id);
            ++structural_version;

            return Get_or_create_storage< COMPONENT_TYPE >()
                .Emplace(_entity, std::forward< Args >(_args)...);
        }

        // Returns the component if the entity already has it, or creates
        // it with the given arguments if it doesn't. Useful for init
        // patterns where you want to ensure a component exists.
        template< typename COMPONENT_TYPE, typename... Args >
        COMPONENT_TYPE& Get_or_add_component(Entity _entity, Args&&... _args)
        {
            if (Has_component< COMPONENT_TYPE >(_entity))
            {
                return Get_component< COMPONENT_TYPE >(_entity);
            }

            return Add_component< COMPONENT_TYPE >(_entity, std::forward< Args >(_args)...);
        }

        // Removes a component from an entity.
        // Returns false (no-op) if the entity is not alive or doesn't have
        // this component type.
        template< typename COMPONENT_TYPE >
        bool Remove_component(Entity _entity)
        {
            if (!Is_alive(_entity)) return false;

            const size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (!Mask_of(_entity).test(type_id)) return false;

            Mask_of(_entity).reset(type_id);

            if (storages[type_id])
            {
                storages[type_id]->Remove(_entity);
            }

            ++structural_version;

            return true;
        }

        // Returns a reference to the component of the given type.
        // Throws std::out_of_range if the entity is not alive or has no
        // component of this type. Use Try_get_component when unsure.
        template< typename COMPONENT_TYPE >
        COMPONENT_TYPE& Get_component(Entity _entity)
        {
            return Get_component_impl< COMPONENT_TYPE >(*this, _entity);
        }

        template< typename COMPONENT_TYPE >
        const COMPONENT_TYPE& Get_component(Entity _entity) const
        {
            return Get_component_impl< COMPONENT_TYPE >(*this, _entity);
        }

        template< typename COMPONENT_TYPE, typename SELF >
        static auto& Get_component_impl(SELF& _self, Entity _entity)
        {
            if (!_self.template Has_component< COMPONENT_TYPE >(_entity))
            {
                throw std::out_of_range(
                    "World::Get_component: entity " + std::to_string(_entity) +
                    " is not alive or has no component of the requested type");
            }

            return Get_storage_impl< COMPONENT_TYPE >(_self).Get(_entity);
        }

        // Returns a pointer to the component if the entity has it,
        // or nullptr if it doesn't (or is not alive). Safe alternative to
        // Get_component when you're not sure whether the entity has the
        // component.
        template< typename COMPONENT_TYPE >
        COMPONENT_TYPE* Try_get_component(Entity _entity)
        {
            return Try_get_component_impl< COMPONENT_TYPE >(*this, _entity);
        }

        template< typename COMPONENT_TYPE >
        const COMPONENT_TYPE* Try_get_component(Entity _entity) const
        {
            return Try_get_component_impl< COMPONENT_TYPE >(*this, _entity);
        }

        template< typename COMPONENT_TYPE, typename SELF >
        static auto* Try_get_component_impl(SELF& _self, Entity _entity)
        {
            using Result = std::conditional_t< std::is_const_v< SELF >, const COMPONENT_TYPE, COMPONENT_TYPE >;

            if (!_self.template Has_component< COMPONENT_TYPE >(_entity)) return static_cast<Result*>(nullptr);

            return Get_storage_impl< COMPONENT_TYPE >(_self).Try_get(_entity);
        }

        // Returns true if the entity is alive and has a component of the
        // given type.
        template< typename COMPONENT_TYPE >
        bool Has_component(Entity _entity) const
        {
            if (!Is_alive(_entity)) return false;

            return Mask_of(_entity).test(Component_Id< COMPONENT_TYPE >());
        }

        // Returns how many entities currently have a component of the given type.
        template< typename COMPONENT_TYPE >
        size_t Entity_count_with() const
        {
            const size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (!storages[type_id]) return 0;

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
            Query_impl< FIRST_COMPONENT, REST_COMPONENTS... >(*this, std::forward< FUNCTION >(_function));
        }

        template< typename FIRST_COMPONENT, typename... REST_COMPONENTS, typename FUNCTION >
        void Query(FUNCTION&& _function) const
        {
            Query_impl< FIRST_COMPONENT, REST_COMPONENTS... >(*this, std::forward< FUNCTION >(_function));
        }

        template< typename FIRST_COMPONENT, typename... REST_COMPONENTS,
            typename SELF, typename FUNCTION >
        static void Query_impl(SELF& _self, FUNCTION&& _function)
        {
            const size_t first_id = Component_Id< FIRST_COMPONENT >();

            if (!_self.storages[first_id]) return;

            // Build required mask: one bit per component type in the query.
            // Any entity whose mask ANDed with required_mask equals
            // required_mask has all the required components.
            Entity_Mask required_mask;
            required_mask.set(first_id);
            (required_mask.set(Component_Id< REST_COMPONENTS >()), ...);

            // Iterate the first type's dense arrays directly - maximally
            // cache-friendly, no indirection for the primary component.
            auto& first_storage = Get_storage_impl< FIRST_COMPONENT >(_self);
            const std::vector< Entity >& entities = first_storage.Get_entities();

            for (size_t i = 0; i < entities.size(); ++i)
            {
                const Entity entity = entities[i];

                // O(1) bitmask check before accessing any other storage
                if ((_self.Mask_of(entity) & required_mask) == required_mask)
                {
                    _function(
                        entity,
                        first_storage.Get_components()[i],
                        Get_storage_impl< REST_COMPONENTS >(_self).Get(entity)...
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
            Each_impl< COMPONENT_TYPE >(*this, std::forward< FUNCTION >(_function));
        }

        template< typename COMPONENT_TYPE, typename FUNCTION >
        void Each(FUNCTION&& _function) const
        {
            Each_impl< COMPONENT_TYPE >(*this, std::forward< FUNCTION >(_function));
        }

        template< typename COMPONENT_TYPE, typename SELF, typename FUNCTION >
        static void Each_impl(SELF& _self, FUNCTION&& _function)
        {
            const size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (!_self.storages[type_id]) return;

            Get_storage_impl< COMPONENT_TYPE >(_self).Each(std::forward< FUNCTION >(_function));
        }

        // =========================================================
        // World management
        // =========================================================

        // Removes all entities and all components. Resets to empty state.
        // Every Entity handed out before the call is dead afterwards (its
        // slot's generation advances), and slot numbering restarts at 0.
        void Clear()
        {
            for (auto& storage : storages)
            {
                if (storage) storage->Clear();
            }

            for (size_t index = 0; index < MAX_ENTITIES; ++index)
            {
                if ((*alive_flags)[index]) ++(*generations)[index];
            }

            entity_masks->fill(Entity_Mask{});
            alive_flags->fill(false);
            alive_entity_count = 0;

            id_provider.Reset();
            ++structural_version;
        }

        // Reorders the component storage of type T so entities appear in
        // _new_order sequence. Both the dense component array and the sparse
        // index map are updated together.
        //
        // _new_order must be a permutation of all entities that currently
        // have a component of type T. Anything else throws
        // std::invalid_argument and leaves the storage untouched.
        //
        // Used by Transform_System to keep Transform_Components in
        // hierarchical order (parents before children), so
        // Transform_System::Update() can run as a single cache-friendly
        // Each() pass without an external sorted list.
        template< typename COMPONENT_TYPE >
        void Reorder_storage(const std::vector<Entity>& _new_order)
        {
            const size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (!storages[type_id])
            {
                if (!_new_order.empty())
                    throw std::invalid_argument("World::Reorder_storage: no storage exists for this component type");
                return;
            }

            Get_storage< COMPONENT_TYPE >().Reorder(_new_order);
        }

    private:

        // =========================================================
        // Access helpers (avoid (*ptr)[i] everywhere)
        // =========================================================

        // Callers guarantee the entity is alive (index in range).
        Entity_Mask& Mask_of(Entity _entity)
        {
            return (*entity_masks)[Entity_index(_entity)];
        }

        const Entity_Mask& Mask_of(Entity _entity) const
        {
            return (*entity_masks)[Entity_index(_entity)];
        }

        bool& Alive_flag_of(Entity _entity)
        {
            return (*alive_flags)[Entity_index(_entity)];
        }

        void Require_alive(Entity _entity, const char* _operation) const
        {
            if (!Is_alive(_entity))
            {
                throw std::invalid_argument(
                    std::string("World::") + _operation + ": entity " +
                    std::to_string(_entity) + " is not alive");
            }
        }

        // =========================================================
        // Component type ID system
        // =========================================================

        // Returns a unique, stable ID for each component type T.
        // The static local is initialized exactly once per type, the
        // first time this function is instantiated for that type.
        // atomic fetch_add ensures thread-safe ID assignment even if
        // types are first used from different threads simultaneously.
        //
        // Throws std::length_error the first time a type past
        // MAX_COMPONENT_TYPES is registered; the id is never handed out,
        // so no array below can be indexed out of range with it.
        template< typename COMPONENT_TYPE >
        static size_t Component_Id()
        {
            static const size_t id = Register_component_id();
            return id;
        }

        static size_t Register_component_id()
        {
            const size_t id = next_component_id.fetch_add(1, std::memory_order_relaxed);

            if (id >= MAX_COMPONENT_TYPES)
            {
                throw std::length_error(
                    "World: more than MAX_COMPONENT_TYPES (" +
                    std::to_string(MAX_COMPONENT_TYPES) + ") component types registered");
            }

            return id;
        }

        // =========================================================
        // Storage management
        // =========================================================

        // Returns the storage for T, creating it if it doesn't exist yet.
        template< typename COMPONENT_TYPE >
        Component_Storage< COMPONENT_TYPE >& Get_or_create_storage()
        {
            const size_t type_id = Component_Id< COMPONENT_TYPE >();

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
            return Get_storage_impl< COMPONENT_TYPE >(*this);
        }

        template< typename COMPONENT_TYPE >
        const Component_Storage< COMPONENT_TYPE >& Get_storage() const
        {
            return Get_storage_impl< COMPONENT_TYPE >(*this);
        }

        // One body for both overloads above. C++23 would write this as
        // `auto&& self`; the project is on C++20, so the same job is done
        // by a static template on SELF and the return type deduces the
        // const-ness from whichever `this` came in.
        //
        // The explicit Result alias is not decoration: storages holds
        // unique_ptr, and operator* on a const unique_ptr still hands back
        // a NON-const referent. Without it, the const path would silently
        // return a mutable storage.
        template< typename COMPONENT_TYPE, typename SELF >
        static auto& Get_storage_impl(SELF& _self)
        {
            const size_t type_id = Component_Id< COMPONENT_TYPE >();

            if (!_self.storages[type_id])
            {
                throw std::logic_error(
                    "World::Get_storage: component type never registered via Add_component");
            }

            using Storage = Component_Storage< COMPONENT_TYPE >;
            using Result = std::conditional_t< std::is_const_v< SELF >, const Storage, Storage >;

            return static_cast<Result&>(*_self.storages[type_id]);
        }

    };

}
