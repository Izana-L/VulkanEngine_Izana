#include <Transform_System.hpp>

#include <World.hpp>
#include <Matrix4.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace EngineCore
{

    namespace
    {
        // Validates the entity for a hierarchy operation.
        ECS::Transform_Component& Require_transform(ECS::Entity _entity, ECS::World& _world, const char* _operation)
        {
            ECS::Transform_Component* transform = _world.Try_get_component<ECS::Transform_Component>(_entity);

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
    // Registration
    // =========================================================

    void Transform_System::Register(ECS::Entity _entity, ECS::World& _world)
    {
        const ECS::Transform_Component& transform = Require_transform(_entity, _world, "Register");

        if (children.find(_entity) != children.end()) return;   // already known

        children[_entity] = {};

        const ECS::Entity parent = transform.Get_parent();

        if (ECS::Is_valid_entity(parent) && _world.Has_component<ECS::Transform_Component>(parent))
            children[parent].push_back(_entity);
        else
            roots.push_back(_entity);

        order_dirty = true;
    }

    void Transform_System::Unregister(ECS::Entity _entity, ECS::World& _world)
    {
        auto it = children.find(_entity);
        if (it == children.end()) return;

        // Its children become roots.
        for (ECS::Entity child : it->second)
        {
            if (ECS::Transform_Component* child_transform = _world.Try_get_component<ECS::Transform_Component>(child))
            {
                child_transform->parent = ECS::INVALID_ENTITY;
                child_transform->dirty = true;
            }

            roots.push_back(child);
        }

        children.erase(it);

        if (ECS::Transform_Component* transform = _world.Try_get_component<ECS::Transform_Component>(_entity))
        {
            Detach_from_cache(_entity, transform->Get_parent());
            transform->parent = ECS::INVALID_ENTITY;
            transform->dirty = true;
        }
        else
        {
            Detach_from_cache(_entity, ECS::INVALID_ENTITY);
        }

        order_dirty = true;
    }

    // =========================================================
    // Hierarchy
    // =========================================================

    void Transform_System::Set_parent(ECS::Entity _child,
        ECS::Entity _parent,
        ECS::World& _world)
    {
        ECS::Transform_Component& child_transform = Require_transform(_child, _world, "Set_parent");

        if (ECS::Is_valid_entity(_parent))
        {
            Require_transform(_parent, _world, "Set_parent");

            if (_parent == _child)
                throw std::invalid_argument("Transform_System::Set_parent: an entity cannot be its own parent");

            // Walking up from the new parent must never reach the child.
            if (Is_same_or_ancestor(_child, _parent, _world))
            {
                throw std::invalid_argument(
                    "Transform_System::Set_parent: entity " + std::to_string(_parent) +
                    " is a descendant of " + std::to_string(_child) + "; the link would create a cycle");
            }
        }

        const ECS::Entity old_parent = child_transform.Get_parent();
        if (old_parent == _parent) return;

        // Everything is validated: mutate the component (source of truth)
        // and keep the cache in step for Get_children().
        if (children.find(_child) == children.end()) children[_child] = {};

        Detach_from_cache(_child, old_parent);

        if (ECS::Is_valid_entity(_parent))
            children[_parent].push_back(_child);
        else
            roots.push_back(_child);

        child_transform.parent = _parent;
        child_transform.dirty = true;

        // Hierarchy changed: reorder the storage so the parent-before-child
        // invariant holds again for the next Update() pass.
        order_dirty = true;
    }

    const std::vector<ECS::Entity>&
        Transform_System::Get_children(ECS::Entity _entity) const
    {
        auto it = children.find(_entity);
        if (it == children.end())
        {
            static const std::vector<ECS::Entity> empty;
            return empty;
        }
        return it->second;
    }

    // =========================================================
    // Per-frame update: single cache-friendly pass
    // =========================================================

    void Transform_System::Update(ECS::World& _world)
    {
        // Any structural change in the world (entities or components
        // created, destroyed, cloned) or any hierarchy operation since the
        // last pass means the cached order may be stale.
        if (order_dirty || seen_world_version != _world.Get_structural_version())
            Synchronize(_world);

        ++update_stamp;
        const uint64_t stamp = update_stamp;

        _world.Each<ECS::Transform_Component>([&](ECS::Entity /*entity*/, ECS::Transform_Component& transform)
        {
            const ECS::Transform_Component* parent_t = nullptr;

            if (transform.Has_parent())
            {
                parent_t = _world.Try_get_component<ECS::Transform_Component>(transform.parent);

                // Cannot happen after Synchronize() (a lost parent is a
                // structural change), kept as a defence: a dangling link
                // is cut rather than followed into a stale matrix.
                if (!parent_t)
                {
                    transform.parent = ECS::INVALID_ENTITY;
                    transform.dirty = true;
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

    void Transform_System::Compute_local_matrix(ECS::Transform_Component& _transform)
    {
        _transform.local_matrix = MathLib::Mat4::TRS(_transform.position, _transform.rotation, _transform.scale);
    }

    void Transform_System::Detach_from_cache(ECS::Entity _entity, ECS::Entity _parent)
    {
        if (ECS::Is_valid_entity(_parent))
        {
            auto parent_it = children.find(_parent);
            if (parent_it != children.end())
            {
                auto& siblings = parent_it->second;
                siblings.erase(std::remove(siblings.begin(), siblings.end(), _entity), siblings.end());
            }
        }

        // Also scrubbed from roots unconditionally: after a dangling link
        // was cut, an entity can be in roots while its component names a
        // parent, and the reverse.
        roots.erase(std::remove(roots.begin(), roots.end(), _entity), roots.end());
    }

    bool Transform_System::Is_same_or_ancestor(ECS::Entity _candidate, ECS::Entity _entity, const ECS::World& _world)
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

    void Transform_System::Synchronize(ECS::World& _world)
    {
        // ── 1. Rebuild the adjacency cache from the components ────
        // Every entity that has a Transform_Component takes part, whether
        // or not it was registered. A parent link to an entity that is
        // dead or has no transform is cut, and the entity becomes a root.
        std::unordered_map<ECS::Entity, std::vector<ECS::Entity>> new_children;
        std::vector<ECS::Entity>                                  new_roots;

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
                    transform.parent = ECS::INVALID_ENTITY;
                    transform.dirty = true;
                }

                new_roots.push_back(entity);
            }
        });

        children = std::move(new_children);
        roots = std::move(new_roots);

        // ── 2. Parent-first order: iterative DFS from every root ──
        std::vector<ECS::Entity> new_order;
        new_order.reserve(transform_count);

        std::unordered_set<ECS::Entity> visited;
        visited.reserve(transform_count);

        std::vector<ECS::Entity> stack;

        for (ECS::Entity root : roots)
        {
            stack.push_back(root);

            while (!stack.empty())
            {
                const ECS::Entity entity = stack.back();
                stack.pop_back();

                if (!visited.insert(entity).second) continue;

                new_order.push_back(entity);

                auto it = children.find(entity);
                if (it == children.end()) continue;

                // Reverse push so children are visited in list order.
                for (auto child = it->second.rbegin(); child != it->second.rend(); ++child)
                    stack.push_back(*child);
            }
        }

        // ── 3. Anything not reached is part of a cycle ────────────
        // Set_parent() rejects cycles, so this is a defence against
        // corruption, not an expected path. Each such entity has its link
        // cut and is appended as a root, so the order stays a permutation
        // of the storage (which is what Reorder_storage requires).
        if (new_order.size() != transform_count)
        {
            _world.Each<ECS::Transform_Component>([&](ECS::Entity entity, ECS::Transform_Component& transform)
            {
                if (visited.count(entity)) return;

                std::cerr << "[Transform_System] Entity " << entity
                    << " is part of a parent cycle; the link is cut and it becomes a root.\n";

                transform.parent = ECS::INVALID_ENTITY;
                transform.dirty = true;
                roots.push_back(entity);
                visited.insert(entity);
                new_order.push_back(entity);
            });
        }

        // ── 4. Physically reorder the component storage ───────────
        _world.Reorder_storage<ECS::Transform_Component>(new_order);

        order_dirty = false;
        seen_world_version = _world.Get_structural_version();
    }

} // namespace EngineCore
