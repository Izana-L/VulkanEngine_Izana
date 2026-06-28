#pragma once

#include <Id.hpp>
#include <Vector.hpp>
#include <Matrix.hpp>
#include <Quaternion.hpp>

namespace ECS
{

    // Transform_Component: stores the position, rotation and scale of an entity
    // in 3D space, plus two cached matrices (local and world) that are
    // recomputed by Transform_System once per frame — never on demand.
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
    //   - parent: CoreTypes::Id of the parent entity. INVALID_ID means root.
    //     Transform_System traverses entities in hierarchical order (parents
    //     before children) so world_matrix is always correct.
    //
    // Access rules:
    //   - Read  position/rotation/scale/matrices directly (public data).
    //   - Write via Set_*() to ensure the dirty flag is set.
    //   - NEVER write to local_matrix / world_matrix directly — those belong
    //     to Transform_System.
    struct Transform_Component
    {
        // =========================================================
        // Transform data
        // =========================================================

        MathLib::Vector3                    position = { 0.0f, 0.0f, 0.0f };
        MathLib::Quat::Quaternion     rotation = MathLib::Quat::Identity();
        MathLib::Vector3                    scale = { 1.0f, 1.0f, 1.0f };

        // =========================================================
        // Hierarchy
        // =========================================================

        // Parent entity ID. INVALID_ID = this is a root entity.
        CoreTypes::Id parent = CoreTypes::INVALID_ID;

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

        // =========================================================
        // Setters — always use these to write transform data
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
        // Direction helpers (derived from rotation, read-only)
        // =========================================================

        // Returns the local forward direction in world space (-Z by convention).
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
    };

} // namespace ECS