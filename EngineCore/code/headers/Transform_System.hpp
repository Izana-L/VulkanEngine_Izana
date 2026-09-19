#pragma once

#include <Transform_Component.hpp>
#include <Id.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ECS { class World; }

namespace EngineCore
{

    // Transform_System: recomputes local and world matrices for all entities
    // that have a Transform_Component, in hierarchical order (parents before
    // children), once per frame.
    //
    // Design — Option 2 (storage ordered hierarchically):
    //   The Component_Storage<Transform_Component> is kept physically sorted
    //   so parents always appear before their children in the dense array.
    //   This means Update() is a single cache-friendly Each() pass — no
    //   external sorted list, no per-entity pointer chasing.
    //
    //   The sort is maintained by calling World::Reorder_storage<Transform_Component>()
    //   whenever the hierarchy changes (Register / Unregister / Set_parent).
    //   This is O(n) in the number of transforms, but happens only on
    //   structural changes, never every frame.
    //
    // Topology structures (owned here, not in Transform_Component):
    //   children: entity → direct children list (adjacency list)
    //   roots:    entities with no parent (starting points for DFS)
    //
    // Thread-safety: none. Call from the main thread.
    class Transform_System
    {
    public:

        Transform_System() = default;
        ~Transform_System() = default;

        Transform_System(const Transform_System&) = delete;
        Transform_System& operator=(const Transform_System&) = delete;

        // =========================================================
        // Registration
        // =========================================================

        // Registers a new entity as a root (no parent).
        // Must be called when an entity with a Transform_Component is created,
        // AFTER Add_component<Transform_Component>() has been called on it.
        void Register(CoreTypes::Id _entity, ECS::World& _world);

        // Unregisters an entity. Its children become roots automatically.
        // Must be called BEFORE the entity's Transform_Component is removed.
        void Unregister(CoreTypes::Id _entity, ECS::World& _world);

        // =========================================================
        // Hierarchy
        // =========================================================

        // Sets _child's parent to _parent.
        // Pass INVALID_ID as _parent to detach (_child becomes a root).
        // Triggers a storage reorder.
        void Set_parent(CoreTypes::Id _child,
            CoreTypes::Id _parent,
            ECS::World& _world);

        // Returns the direct children of _entity (empty if none or not registered).
        const std::vector<CoreTypes::Id>&
            Get_children(CoreTypes::Id _entity) const;

        // =========================================================
        // Per-frame update
        // =========================================================

        // Recomputes local_matrix and world_matrix for every dirty
        // Transform_Component in a single cache-friendly Each() pass.
        // Because the storage is kept in parent-before-child order,
        // parent.world_matrix is always valid before the child reads it.
        // Call once per frame after gameplay and before Extract.
        void Update(ECS::World& _world);

    private:

        // =========================================================
        // Internal helpers
        // =========================================================

        // Recomputes local_matrix from position, rotation, scale (TRS).
        static void Compute_local_matrix(ECS::Transform_Component& _transform);

        // Rebuilds sorted_order via DFS from all roots, then calls
        // World::Reorder_storage<Transform_Component>(sorted_order).
        void Rebuild_and_apply_order(ECS::World& _world);

        // Recursive DFS visit used by Rebuild_and_apply_order.
        void Visit(CoreTypes::Id _entity, std::vector<CoreTypes::Id>& _out_order) const;

        // Returns true if _ancestor is an ancestor of _entity (debug only).
        bool Is_ancestor_of(CoreTypes::Id _ancestor, CoreTypes::Id _entity) const;

        // =========================================================
        // Data
        // =========================================================

        // Adjacency list: entity  direct children.
        std::unordered_map<CoreTypes::Id, std::vector<CoreTypes::Id>> children;

        // Root entities (parent == INVALID_ID).
        std::vector<CoreTypes::Id> roots;

       
        std::unordered_set<CoreTypes::Id> moved_this_frame;
    };

} // namespace EngineCore