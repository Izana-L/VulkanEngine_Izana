#include <Transform_System.hpp>

#include <World.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cassert>

namespace EngineCore
{

    // =========================================================
    // Registration
    // =========================================================

    void Transform_System::Register(CoreTypes::Id _entity, ECS::World& _world)
    {
        assert(CoreTypes::Is_valid(_entity) &&"Transform_System::Register: invalid entity id");
        assert(children.find(_entity) == children.end() && "Transform_System::Register: entity already registered");
        assert(_world.Has_component<ECS::Transform_Component>(_entity) && "Transform_System::Register: entity has no Transform_Component — "
                                                                          "call Add_component<Transform_Component>() first");

        children[_entity] = {};
        roots.push_back(_entity);

        // Reorder storage — new entity is always a root, so it goes at the end
        // of whatever subtree it belongs to. A full rebuild is safe and simple.
        Rebuild_and_apply_order(_world);
    }

    void Transform_System::Unregister(CoreTypes::Id _entity, ECS::World& _world)
    {
        assert(CoreTypes::Is_valid(_entity) && "Transform_System::Unregister: invalid entity id");
            
        assert(children.find(_entity) != children.end() && "Transform_System::Unregister: entity was never registered ");

        ECS::Transform_Component* transform = _world.Try_get_component<ECS::Transform_Component>(_entity);

        if (transform && CoreTypes::Is_valid(transform->parent))
        {
            auto parent_it = children.find(transform->parent);
            if (parent_it != children.end())
            {
                auto& siblings = parent_it->second;
                siblings.erase(std::remove(siblings.begin(), siblings.end(), _entity),siblings.end());
            }
        }
        else
        {
            roots.erase(std::remove(roots.begin(), roots.end(), _entity),roots.end());
        }

        // ── Sus hijos pasan a ser raices ─────────────────────────
        auto it = children.find(_entity);
        if (it != children.end())
        {
            for (CoreTypes::Id child : it->second)
            {
                ECS::Transform_Component* child_transform = _world.Try_get_component<ECS::Transform_Component>(child);

                if (child_transform)
                {
                    child_transform->parent = CoreTypes::INVALID_ID;
                    child_transform->dirty = true;
                }

                roots.push_back(child);
            }
            children.erase(it);
        }

        _world.Remove_component<ECS::Transform_Component>(_entity);

        Rebuild_and_apply_order(_world);
    }

    // =========================================================
    // Hierarchy
    // =========================================================

    void Transform_System::Set_parent(CoreTypes::Id _child,
        CoreTypes::Id _parent,
        ECS::World& _world)
    {
        assert(CoreTypes::Is_valid(_child) && "Transform_System::Set_parent: invalid child id");
        assert(children.find(_child) != children.end() && "Transform_System::Set_parent: child not registered");

        ECS::Transform_Component* child_transform =
            _world.Try_get_component<ECS::Transform_Component>(_child);
        assert(child_transform && "Transform_System::Set_parent: child has no Transform_Component");

        const CoreTypes::Id old_parent = child_transform->parent;
        if (old_parent == _parent) return;

        assert(!CoreTypes::Is_valid(_parent) || !Is_ancestor_of(_child, _parent) && "Transform_System::Set_parent: would create a cycle in the hierarchy");

        // ── Remove from old parent or roots ───────────────────────
        if (CoreTypes::Is_valid(old_parent))
        {
            auto& siblings = children[old_parent];
            siblings.erase(
                std::remove(siblings.begin(), siblings.end(), _child),
                siblings.end());
        }
        else
        {
            roots.erase(
                std::remove(roots.begin(), roots.end(), _child),
                roots.end());
        }

        // ── Attach to new parent or roots ─────────────────────────
        child_transform->parent = _parent;

        if (CoreTypes::Is_valid(_parent))
        {
            assert(children.find(_parent) != children.end() && "Transform_System::Set_parent: parent not registered");
            children[_parent].push_back(_child);
        }
        else
        {
            roots.push_back(_child);
        }

        child_transform->dirty = true;

        // Hierarchy changed → reorder the storage so the parent-before-child
        // invariant holds again for the next Update() pass.
        Rebuild_and_apply_order(_world);
    }

    const std::vector<CoreTypes::Id>&
        Transform_System::Get_children(CoreTypes::Id _entity) const
    {
        auto it = children.find(_entity);
        if (it == children.end())
        {
            static const std::vector<CoreTypes::Id> empty;
            return empty;
        }
        return it->second;
    }

    // =========================================================
    // Per-frame update — single cache-friendly pass
    // =========================================================

    void Transform_System::Update(ECS::World& _world)
    {
       
        moved_this_frame.clear();

        _world.Each<ECS::Transform_Component>([&](ECS::Entity entity, ECS::Transform_Component& transform)
        {
                const bool has_parent = CoreTypes::Is_valid(transform.parent);

                
                const ECS::Transform_Component* parent_t = has_parent ? _world.Try_get_component<ECS::Transform_Component>(transform.parent) : nullptr;

               
                const bool parent_moved = has_parent && moved_this_frame.count(transform.parent) != 0;

                if (!transform.dirty && !parent_moved) return;

                Compute_local_matrix(transform);

                transform.world_matrix = parent_t ? parent_t->world_matrix * transform.local_matrix : transform.local_matrix;
                    
                    

                transform.dirty = false;
                moved_this_frame.insert(entity);
        });
    }

    // =========================================================
    // Internal helpers
    // =========================================================

    void Transform_System::Compute_local_matrix(ECS::Transform_Component& _transform)
    {
        // TRS: T * R * S
        const MathLib::Matrix4 t = glm::translate(MathLib::Matrix4(1.0f), _transform.position);
        const MathLib::Matrix4 r = glm::mat4_cast(_transform.rotation);
        const MathLib::Matrix4 s = glm::scale(MathLib::Matrix4(1.0f), _transform.scale);

        _transform.local_matrix = t * r * s;
    }

    void Transform_System::Rebuild_and_apply_order(ECS::World& _world)
    {
        std::vector<CoreTypes::Id> new_order;
        new_order.reserve(children.size());

        // DFS from every root — guarantees parents before children.
        for (CoreTypes::Id root : roots)
            Visit(root, new_order);

        // Physically reorder the component storage.
        _world.Reorder_storage<ECS::Transform_Component>(new_order);
    }

    void Transform_System::Visit(CoreTypes::Id              _entity,
        std::vector<CoreTypes::Id>& _out_order) const
    {
        _out_order.push_back(_entity);

        auto it = children.find(_entity);
        if (it == children.end()) return;

        for (CoreTypes::Id child : it->second)
            Visit(child, _out_order);
    }

    bool Transform_System::Is_ancestor_of(CoreTypes::Id _ancestor,
        CoreTypes::Id _entity) const
    {
        // Walk up from _entity — only used in debug asserts.
        CoreTypes::Id current = _entity;

        while (CoreTypes::Is_valid(current))
        {
            if (current == _ancestor) return true;

            bool found = false;
            for (const auto& [parent, child_list] : children)
            {
                for (CoreTypes::Id child : child_list)
                {
                    if (child == current)
                    {
                        current = parent;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (!found) break;
        }
        return false;
    }

} // namespace EngineCore