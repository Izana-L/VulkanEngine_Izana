#pragma once

#include <Entity.hpp>

#include <cstddef>

namespace ECS
{

    // IComponent_Storage: type-erased base interface for all
    // Component_Storage<T> instances. Allows World to store and
    // operate on component storages of different types through a
    // common pointer (std::unique_ptr<IComponent_Storage>), without
    // knowing the concrete component type T at the call site.
    //
    // Only operations that World needs to perform on a storage without
    // knowing T are declared here - type-specific operations (Get,
    // Emplace, Each...) are only accessible after casting back to
    // Component_Storage<T>, which World does internally when it knows
    // the type via template parameters.
    class IComponent_Storage
    {
    public:

        virtual ~IComponent_Storage() = default;

        // Returns true if the given entity has a component in this storage.
        // Used by World to check component presence without knowing T,
        // for example when destroying an entity (must remove from all
        // storages that contain it).
        virtual bool Has(Entity _entity) const = 0;

        // Removes the component of the given entity from this storage.
        // Used by World::Destroy_entity to clean up all components of
        // an entity across every storage, without knowing T.
        virtual void Remove(Entity _entity) = 0;

        // Removes all components from this storage.
        // Used by World::Clear to reset the entire world state.
        virtual void Clear() = 0;

        // Returns the number of entities that have a component in this storage.
        virtual size_t Size() const = 0;
        // Copies the component of _source into _destination within
        // this storage. Used by World::Clone_entity to duplicate all
        // components of an entity without knowing their concrete types.
        // Precondition: _source must have a component in this storage.
        virtual void Clone_to(Entity _source, Entity _destination) = 0;

    };

}