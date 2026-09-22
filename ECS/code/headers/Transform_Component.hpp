#pragma once

#include <Entity.hpp>
#include <Vector.hpp>
#include <Vector3.hpp>
#include <Matrix.hpp>
#include <Matrix4.hpp>
#include <Quaternion.hpp>

#include <cstdint>

namespace EngineCore { class Transform_System; }

namespace ECS
{

    // Transform_Component: stores the position, rotation and scale of an entity
    // in 3D space, plus two cached matrices (local and world) that are
    // recomputed by Transform_System once per frame, never on demand.
    //
    // Design decisions:
    //   - Rotation is stored as a quaternion to avoid gimbal lock and for
    //     cheap interpolation (Slerp/Nlerp).
    //   - local_matrix = TRS(position, rotation, scale).
    //   - world_matrix = parent.world_matrix * local_matrix
    //     (identity * local if no parent).
    //   - dirty flag: set automatically when position/rotation/scale change
    //     via the setters. Transform_System only recalculates dirty nodes,
    //     so static objects pay zero cost after the first frame.
    //   - parent: Entity of the parent. INVALID_ENTITY means root.
    //     Transform_System traverses entities in hierarchical order (parents
    //     before children) so world_matrix is always correct.
    //
    // Access rules:
    //   - Read  position/rotation/scale/matrices directly (public data).
    //   - Write via Set_*() to ensure the dirty flag is set.
    //   - NEVER write to local_matrix / world_matrix directly; those belong
    //     to Transform_System.
    //   - The parent link is read through Get_parent() and can only be
    //     changed by Transform_System::Set_parent(), which also maintains
    //     the adjacency data the hierarchical update depends on. That is
    //     why the field itself is private.
    //
    // Two families of direction helpers exist and they are NOT the same:
    //   - Forward()/Right()/Up() are derived from `rotation` alone, i.e.
    //     they are expressed in the PARENT's space (world space only for
    //     a root entity).
    //   - World_forward()/World_right()/World_up()/World_position() are
    //     derived from world_matrix and are valid after the last
    //     Transform_System::Update(). Anything that renders or lights
    //     (cameras, lights) must use these.
    struct Transform_Component
    {
        // =========================================================
        // Transform data
        // =========================================================

        MathLib::Vector3          position = { 0.0f, 0.0f, 0.0f };
        MathLib::Quat::Quaternion rotation = MathLib::Quat::Identity();
        MathLib::Vector3          scale = { 1.0f, 1.0f, 1.0f };

        // =========================================================
        // Cached matrices (written only by Transform_System)
        // =========================================================

        MathLib::Matrix4 local_matrix = MathLib::Matrix4(1.0f);
        MathLib::Matrix4 world_matrix = MathLib::Matrix4(1.0f);

        // =========================================================
        // Dirty flag
        // =========================================================

        // True when position, rotation or scale changed since the last
        // Transform_System pass. Set_*() setters mark this automatically.
        // Transform_System clears it after recomputing the matrices.
        bool dirty = true;   // starts dirty so the first frame always computes

        // Stamp of the Transform_System pass that last recomputed
        // world_matrix. Written only by Transform_System; it is how a
        // child learns, in O(1), that its parent moved during the current
        // pass.
        uint64_t last_world_update = 0;

        // =========================================================
        // Hierarchy (read-only outside Transform_System)
        // =========================================================

        // Parent entity. INVALID_ENTITY = this is a root entity.
        Entity Get_parent() const
        {
            return parent;
        }

        bool Has_parent() const
        {
            return Is_valid_entity(parent);
        }

        // =========================================================
        // Setters - always use these to write transform data
        // =========================================================

        void Set_position(const MathLib::Vector3& _position)
        {
            position = _position;
            dirty = true;
        }

        void Set_rotation(const MathLib::Quat::Quaternion& _rotation)
        {
            rotation = MathLib::Quat::Normalize(_rotation);
            dirty = true;
        }

        void Set_scale(const MathLib::Vector3& _scale)
        {
            scale = _scale;
            dirty = true;
        }

        // Convenience: set rotation from Euler angles (pitch, yaw, roll) in radians.
        void Set_rotation_euler(float _pitch, float _yaw, float _roll)
        {
            rotation = MathLib::Quat::Normalize(
                MathLib::Quat::From_euler(_pitch, _yaw, _roll));
            dirty = true;
        }

        void Set_rotation_euler(const MathLib::Vector3& _euler_radians)
        {
            rotation = MathLib::Quat::Normalize(
                MathLib::Quat::From_euler(_euler_radians));
            dirty = true;
        }

        // Convenience: set all three at once (avoids three separate dirty marks).
        void Set(const MathLib::Vector3& _position,
            const MathLib::Quat::Quaternion& _rotation,
            const MathLib::Vector3& _scale)
        {
            position = _position;
            rotation = MathLib::Quat::Normalize(_rotation);
            scale = _scale;
            dirty = true;
        }

        // =========================================================
        // Direction helpers in PARENT space (derived from rotation)
        // =========================================================

        // Local forward direction (-Z by convention) rotated by `rotation`.
        // Equal to the world direction only for a root entity.
        MathLib::Vector3 Forward() const
        {
            return MathLib::Quat::GetForward(rotation);
        }

        MathLib::Vector3 Right() const
        {
            return MathLib::Quat::GetRight(rotation);
        }

        MathLib::Vector3 Up() const
        {
            return MathLib::Quat::GetUp(rotation);
        }

        // =========================================================
        // WORLD-space helpers (derived from world_matrix)
        // =========================================================

        // Valid after Transform_System::Update() has run for this frame.
        MathLib::Vector3 World_position() const
        {
            return MathLib::Mat4::Get_translation(world_matrix);
        }

        MathLib::Vector3 World_forward() const
        {
            return MathLib::Mat4::Transform_direction(world_matrix, MathLib::Vector3(0.0f, 0.0f, -1.0f));
        }

        MathLib::Vector3 World_right() const
        {
            return MathLib::Mat4::Transform_direction(world_matrix, MathLib::Vector3(1.0f, 0.0f, 0.0f));
        }

        MathLib::Vector3 World_up() const
        {
            return MathLib::Mat4::Transform_direction(world_matrix, MathLib::Vector3(0.0f, 1.0f, 0.0f));
        }

    private:

        // Only Transform_System writes this: it must also update its
        // adjacency lists, or world_matrix would silently go stale.
        Entity parent = INVALID_ENTITY;

        friend class EngineCore::Transform_System;
    };

} // namespace ECS
