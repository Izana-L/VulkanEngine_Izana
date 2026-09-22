#pragma once

#include <Transform_Component.hpp>
#include <Entity.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ECS { class World; }

namespace EngineCore
{

    // Transform_System: recomputes local and world matrices for all entities
    // that have a Transform_Component, in hierarchical order (parents before
    // children), once per frame.
    //
    // Design: storage ordered hierarchically.
    //   The Component_Storage<Transform_Component> is kept physically sorted
    //   so parents always appear before their children in the dense array.
    //   This means Update() is a single cache-friendly Each() pass: no
    //   external sorted list, no per-entity pointer chasing.
    //
    // Source of truth: the `parent` field of each Transform_Component
    //   (written only by Set_parent(), through friendship). The adjacency
    //   lists kept here (children, roots) are a cache derived from it, and
    //   they are rebuilt from the components whenever the world's set of
    //   entities or components changed (World::Get_structural_version) or
    //   a hierarchy operation ran. As a consequence:
    //     - an entity that gets a Transform_Component without being
    //       registered here is still updated (as a root, or under the
    //       parent its component names);
    //     - a cloned entity keeps its source's parent link and is ordered
    //       correctly;
    //     - an entity destroyed through World::Destroy_entity() is pruned
    //       and its children become roots, without any callback.
    //   Register() / Unregister() therefore only maintain the cache eagerly
    //   so Get_children() is current between two Update() calls.
    //
    // Every invariant that keeps the storage reorder valid (the order is a
    // permutation of the storage) is enforced in every build; the storage
    // itself rejects anything else with an exception.
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
        // Registration (optional, see class comment)
        // =========================================================

        // Adds an entity with a Transform_Component to the hierarchy cache,
        // as a root or under the parent its component already names.
        // Idempotent. Throws std::invalid_argument if the entity is not
        // alive or has no Transform_Component.
        void Register(ECS::Entity _entity, ECS::World& _world);

        // Detaches an entity from the hierarchy: its children become roots
        // and it is removed from the cache. Its Transform_Component is left
        // in place (it will be treated as a root if it stays). No-op if the
        // entity is unknown.
        void Unregister(ECS::Entity _entity, ECS::World& _world);

        // =========================================================
        // Hierarchy
        // =========================================================

        // Sets _child's parent to _parent.
        // Pass INVALID_ENTITY as _parent to detach (_child becomes a root).
        // Throws std::invalid_argument if either entity is not alive or has
        // no Transform_Component, or if the link would create a cycle.
        void Set_parent(ECS::Entity _child,
            ECS::Entity _parent,
            ECS::World& _world);

        // Returns the direct children of _entity as of the last Update() or
        // hierarchy operation (empty if none or unknown).
        const std::vector<ECS::Entity>&
            Get_children(ECS::Entity _entity) const;

        // =========================================================
        // Per-frame update
        // =========================================================

        // Recomputes local_matrix and world_matrix for every dirty
        // Transform_Component (and every child of a moved parent) in a
        // single cache-friendly Each() pass. Because the storage is kept
        // in parent-before-child order, parent.world_matrix is always
        // valid before the child reads it.
        // Call once per frame after gameplay and before Extract.
        void Update(ECS::World& _world);

    private:

        // =========================================================
        // Internal helpers
        // =========================================================

        // Recomputes local_matrix from position, rotation, scale (TRS).
        static void Compute_local_matrix(ECS::Transform_Component& _transform);

        // Rebuilds children/roots from the components (pruning dead
        // entities and dangling parent links), computes the parent-first
        // order and applies it to the storage.
        void Synchronize(ECS::World& _world);

        // True if _candidate is _entity itself or one of its ancestors,
        // following the components' parent links.
        static bool Is_same_or_ancestor(ECS::Entity _candidate, ECS::Entity _entity, const ECS::World& _world);

        // Removes _entity from its parent's children list or from roots.
        void Detach_from_cache(ECS::Entity _entity, ECS::Entity _parent);

        // =========================================================
        // Data
        // =========================================================

        // Adjacency list: entity -> direct children.
        std::unordered_map<ECS::Entity, std::vector<ECS::Entity>> children;

        // Root entities (parent == INVALID_ENTITY).
        std::vector<ECS::Entity> roots;

        // Set by every hierarchy operation; cleared by Synchronize().
        bool order_dirty = true;

        // World::Get_structural_version() seen by the last Synchronize().
        uint64_t seen_world_version = 0;

        // Incremented per Update(); stamped into each recomputed transform
        // so children detect a parent recomputed in the same pass.
        uint64_t update_stamp = 0;
    };

} // namespace EngineCore
