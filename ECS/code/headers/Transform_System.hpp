#pragma once

#include <Transform_Component.hpp>
#include <Entity.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>



namespace ECS
{
    class World;
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
    //   (written only by Set_parent(), through friendship). Nothing has to
    //   be registered here: the hierarchy is rebuilt from the components by
    //   the next Update() whenever the world's set of entities or
    //   components changed (World::Get_structural_version) or Set_parent()
    //   ran. As a consequence:
    //     - an entity that gets a Transform_Component is updated (as a
    //       root, or under the parent its component names);
    //     - a cloned entity keeps its source's parent link and is ordered
    //       correctly;
    //     - an entity destroyed through World::Destroy_entity() is pruned
    //       and its children become roots, without any callback.
    //   The children lists behind Get_children() are the snapshot taken by
    //   that rebuild; no operation edits them by hand.
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
        // Hierarchy
        // =========================================================

        // Sets _child's parent to _parent.
        // Pass INVALID_ENTITY as _parent to detach (_child becomes a root).
        // Throws std::invalid_argument if either entity is not alive or has
        // no Transform_Component, or if the link would create a cycle
        // (which includes making an entity its own parent).
        void Set_parent(Entity _child, Entity _parent, World& _world);

        // Returns the direct children of _entity as of the last Update()
        // (empty if none or unknown). A Set_parent() call or a structural
        // change of the world shows up from the next Update().
        const std::vector<Entity>& Get_children(Entity _entity) const;

        // =========================================================
        // Per-frame update
        // =========================================================

        // Recomputes local_matrix and world_matrix for every dirty
        // Transform_Component (and every child of a moved parent) in a
        // single cache-friendly Each() pass. Because the storage is kept
        // in parent-before-child order, parent.world_matrix is always
        // valid before the child reads it.
        // Call once per frame after gameplay and before Extract.
        void Update(World& _world);

    private:

        // =========================================================
        // Internal helpers
        // =========================================================

        // Recomputes local_matrix from position, rotation, scale (TRS).
        static void Compute_local_matrix(Transform_Component& _transform);

        // Makes _transform a root and marks it dirty, so its world matrix is
        // recomputed without the parent it lost. The one place that cuts a
        // link: the caller sets order_dirty when the order may be stale.
        static void Cut_parent_link(Transform_Component& _transform);

        // Rebuilds the children lists from the components (pruning dead
        // entities and dangling parent links), computes the parent-first
        // order and applies it to the storage.
        void Synchronize(World& _world);

        // True if _candidate is _entity itself or one of its ancestors,
        // following the components' parent links.
        static bool Is_same_or_ancestor(Entity _candidate, Entity _entity, const World& _world);

        // =========================================================
        // Data
        // =========================================================

        // Adjacency list: entity -> direct children. A snapshot written
        // only by Synchronize() and read only by Get_children().
        std::unordered_map<Entity, std::vector<Entity>> children;

        // Set by Set_parent() and by the dangling-link defence of Update();
        // cleared by Synchronize().
        bool order_dirty = true;

        // World::Get_structural_version() seen by the last Synchronize().
        uint64_t seen_world_version = 0;

        // Incremented per Update(); stamped into each recomputed transform
        // so children detect a parent recomputed in the same pass.
        uint64_t update_stamp = 0;
    };

} 
