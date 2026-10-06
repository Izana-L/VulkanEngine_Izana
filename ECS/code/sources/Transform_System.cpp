#include <Transform_System.hpp>

#include <World.hpp>
#include <Matrix4.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace ECS
{

    namespace
    {
        // Validates the entity for a hierarchy operation.
        Transform_Component& Require_transform(Entity _entity, World& _world, const char* _operation)
        {
            Transform_Component* transform = _world.Try_get_component<Transform_Component>(_entity);

            if (!transform)
            {
                throw std::invalid_argument(
                    std::string("Transform_System::") + _operation + ": entity " + std::to_string(_entity) +
                    " is not alive or has no Transform_Component");
            }

            return *transform;
        }
    }

    // =========================================================
    // Hierarchy
    // =========================================================

    void Transform_System::Set_parent(Entity _child,
        Entity _parent,
        World& _world)
    {
       Transform_Component& child_transform = Require_transform(_child, _world, "Set_parent");

        if (Is_valid_entity(_parent))
        {
            Require_transform(_parent, _world, "Set_parent");

            // Walking up from the new parent must never reach the child.
            // The walk starts at _parent itself, so this also rejects an
            // entity as its own parent.
            if (Is_same_or_ancestor(_child, _parent, _world))
            {
                throw std::invalid_argument(
                    "Transform_System::Set_parent: making entity " + std::to_string(_parent) +
                    " the parent of entity " + std::to_string(_child) + " would create a cycle");
            }
        }

        // Any "no entity" value means detach; store the canonical sentinel so
        // `parent == INVALID_ENTITY` comparisons keep working.
        const Entity new_parent = Is_valid_entity(_parent) ? _parent : INVALID_ENTITY;

        if (child_transform.Get_parent() == new_parent) return;

        // Everything is validated: mutate the component (source of truth).
        child_transform.parent = new_parent;
        child_transform.dirty = true;

        // Hierarchy changed: reorder the storage so the parent-before-child
        // invariant holds again for the next Update() pass.
        order_dirty = true;
    }

    const std::vector<Entity>&
        Transform_System::Get_children(Entity _entity) const
    {
        auto it = children.find(_entity);
        if (it == children.end())
        {
            static const std::vector<Entity> empty;
            return empty;
        }
        return it->second;
    }

    // =========================================================
    // Per-frame update: single cache-friendly pass
    // =========================================================

    void Transform_System::Update(World& _world)
    {
        // Any structural change in the world (entities or components
        // created, destroyed, cloned) or any hierarchy operation since the
        // last pass means the cached order may be stale.
        if (order_dirty || seen_world_version != _world.Get_structural_version())
            Synchronize(_world);

        ++update_stamp;
        const uint64_t stamp = update_stamp;

        _world.Each<Transform_Component>([&](Entity /*entity*/, Transform_Component& transform)
        {
            const Transform_Component* parent_t = nullptr;

            if (transform.Has_parent())
            {
                parent_t = _world.Try_get_component<Transform_Component>(transform.parent);

                // Cannot happen after Synchronize() (a lost parent is a
                // structural change), kept as a defence: a dangling link
                // is cut rather than followed into a stale matrix.
                if (!parent_t)
                {
                    Cut_parent_link(transform);
                    order_dirty = true;
                }
            }

            // A parent recomputed earlier in THIS pass carries the current
            // stamp; parents always precede children in the storage.
            const bool parent_moved = parent_t && parent_t->last_world_update == stamp;

            if (!transform.dirty && !parent_moved) return;

            Compute_local_matrix(transform);

            transform.world_matrix = parent_t
                ? parent_t->world_matrix * transform.local_matrix
                : transform.local_matrix;

            transform.dirty = false;
            transform.last_world_update = stamp;
        });
    }

    // =========================================================
    // Internal helpers
    // =========================================================

    void Transform_System::Compute_local_matrix(Transform_Component& _transform)
    {
        _transform.local_matrix = MathLib::Mat4::TRS(_transform.position, _transform.rotation, _transform.scale);
    }

    void Transform_System::Cut_parent_link(Transform_Component& _transform)
    {
        _transform.parent = ECS::INVALID_ENTITY;
        _transform.dirty = true;
    }

    bool Transform_System::Is_same_or_ancestor(Entity _candidate, Entity _entity, const World& _world)
    {
        // Bounded by the number of live entities: a chain longer than that
        // can only be a cycle, which this function exists to prevent.
        size_t steps = _world.Entity_count() + 1;
        ECS::Entity current = _entity;

        while (ECS::Is_valid_entity(current) && steps-- > 0)
        {
            if (current == _candidate) return true;

            const ECS::Transform_Component* transform = _world.Try_get_component<ECS::Transform_Component>(current);
            if (!transform) break;

            current = transform->Get_parent();
        }

        return false;
    }

    void Transform_System::Synchronize(World& _world)
    {
        // ── 1. Rebuild the adjacency lists from the components ────
        // Every entity that has a Transform_Component takes part. A parent
        // link to an entity that is dead or has no transform is cut, and the
        // entity becomes a root.
        std::unordered_map<ECS::Entity, std::vector<ECS::Entity>> new_children;
        std::vector<ECS::Entity>                                  roots;

        size_t transform_count = 0;

        _world.Each<ECS::Transform_Component>([&](ECS::Entity entity, ECS::Transform_Component& transform)
        {
            ++transform_count;
            new_children.try_emplace(entity);

            const ECS::Entity parent = transform.Get_parent();

            if (ECS::Is_valid_entity(parent) && _world.Has_component<ECS::Transform_Component>(parent))
            {
                new_children[parent].push_back(entity);
            }
            else
            {
                if (ECS::Is_valid_entity(parent))
                {
                    // Dangling link (parent destroyed or stripped of its
                    // transform): this entity is a root from now on.
                    Cut_parent_link(transform);
                }

                roots.push_back(entity);
            }
        });

        children = std::move(new_children);

        // ── 2. Parent-first order: iterative DFS from every root ──
        std::vector<Entity> new_order;
        new_order.reserve(transform_count);

        std::unordered_set<Entity> visited;
        visited.reserve(transform_count);

        std::vector<Entity> stack;

        // Appends _root and its whole subtree to new_order, parents first.
        const auto visit_subtree = [&](Entity _root)
        {
            stack.push_back(_root);

            while (!stack.empty())
            {
                const Entity entity = stack.back();
                stack.pop_back();

                if (!visited.insert(entity).second) continue;

                new_order.push_back(entity);

                auto it = children.find(entity);
                if (it == children.end()) continue;

                // Reverse push so children are visited in list order.
                for (auto child = it->second.rbegin(); child != it->second.rend(); ++child)
                    stack.push_back(*child);
            }
        };

        for (Entity root : roots) visit_subtree(root);

        // ── 3. Anything not reached hangs from a cycle ────────────
        // Set_parent() rejects cycles, so this is a defence against
        // corruption, not an expected path.
        //
        // A transform whose parent was visited is itself visited (the parent
        // pushed all its children), so every unreached transform has an
        // UNREACHED parent. In that graph each connected component holds
        // exactly one cycle, with trees hanging from it. Cutting ONE link per
        // cycle makes the cycle's node a root and lets the DFS reach the whole
        // component: members of the cycle keep their other links and the
        // descendants that merely hang from it keep their parents. The order
        // stays a permutation of the storage, which Reorder_storage requires.
        if (new_order.size() != transform_count)
        {
            std::unordered_set<Entity> walk;

            _world.Each<Transform_Component>([&](Entity entity, Transform_Component&)
            {
                if (visited.count(entity)) return;

                // Climb parent links until a node repeats: that node is ON the
                // cycle. (The null/no-parent exits cannot happen here; they only
                // keep the loop total if the invariant above is ever broken.)
                walk.clear();
                Entity cursor = entity;

                while (walk.insert(cursor).second)
                {
                    const Transform_Component* t = _world.Try_get_component<Transform_Component>(cursor);
                    if (!t || !t->Has_parent()) break;
                    cursor = t->Get_parent();
                }

                Transform_Component* cut = _world.Try_get_component<Transform_Component>(cursor);

                if (cut && cut->Has_parent())
                {
                    std::cerr << "[Transform_System] Entity " << cursor
                        << " is part of a parent cycle; its link to " << cut->Get_parent()
                        << " is cut and it becomes a root.\n";

                    // Keep the adjacency snapshot in step with the component.
                    std::vector<Entity>& siblings = children[cut->Get_parent()];
                    siblings.erase(std::remove(siblings.begin(), siblings.end(), cursor), siblings.end());

                    Cut_parent_link(*cut);
                }

                visit_subtree(cursor);
            });
        }

        // ── 4. Physically reorder the component storage ───────────
        _world.Reorder_storage<Transform_Component>(new_order);

        order_dirty = false;
        seen_world_version = _world.Get_structural_version();
    }

} 
