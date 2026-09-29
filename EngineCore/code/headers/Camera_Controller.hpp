#pragma once

#include <Entity.hpp>
#include <Input.hpp>
#include <Quaternion.hpp>

namespace ECS { class World; }


namespace EngineCore
{


    // Camera_Controller: FPS-style free-flight camera controller.
    //
    // Operates on the Transform_Component of a camera entity (an entity
    // with Transform_Component + Camera_Component). Reads named actions
    // from Input, never raw keys, so controls can be remapped via the
    // input JSON without touching this code.
    //
    // Rotation model: the Transform's rotation is the source of truth;
    // yaw and pitch are a cache of it, validated on every Update():
    //   - The controller remembers the last rotation it wrote. When the
    //     Transform holds a different one, other code (a cutscene, a
    //     teleport, an editor) wrote it, and yaw/pitch are derived again
    //     from the Transform, so mouse input continues from that rotation
    //     without a jump.
    //   - The rotation is rebuilt from yaw and pitch and written only when
    //     they changed (mouse input in camera mode) or on the first update.
    //     Without input nothing is written and the Transform is not marked
    //     dirty, so a still camera costs no transform recomputation.
    // Rebuilding the quaternion from the two angles, instead of composing
    // incremental rotations with the stored one, keeps it free of drift;
    // the angles are only re-derived after an external write, so they do
    // not drift either.
    //
    // Roll is not supported: a rolled rotation (initial or written by
    // other code) is projected onto the yaw/pitch model, and its roll is
    // lost the next time the controller writes the rotation.
    //
    // The transform written is the camera's LOCAL rotation/position, as
    // for any other entity; the Extractor derives the view from the world
    // matrix, so a camera parented to a pivot behaves like any child.
    //
    // Rotation only happens while Input is in Cursor_Mode::Camera (cursor
    // captured). The "ToggleCamera" action switches between camera and
    // window cursor modes.
    //
    // Actions consumed (must exist in the input JSON):
    //   MoveForward, MoveBack, MoveLeft, MoveRight, MoveUp, MoveDown,
    //   Sprint, ToggleCamera
    class Camera_Controller
    {
    public:

        Camera_Controller() = default;
        ~Camera_Controller() = default;

        // =========================================================
        // Update
        // =========================================================

        // Updates the camera entity's Transform from input.
        // _camera_entity must have a Transform_Component; otherwise the
        // call does nothing.
        // _dt is the frame delta time in seconds.
        void Update(ECS::Entity _camera_entity,
            Input_System::Input& _input,
            ECS::World& _world,
            float         _dt);

        // =========================================================
        // Tuning parameters (public: adjust freely)
        // =========================================================

        // Movement speed in world units per second.
        float move_speed = 5.0f;

        // Multiplier applied to move_speed while the Sprint action is held.
        float sprint_multiplier = 3.0f;

        // Radians of rotation per pixel of mouse movement.
        float mouse_sensitivity = 0.002f;

        // Pitch clamp in radians (default ~89 degrees) to prevent the
        // camera from flipping over at the poles.
        float pitch_limit = 1.553343f;   // 89 degrees in radians

    private:
        struct Action_Ids
        {
            size_t move_forward = Input_System::Input::INVALID_ACTION;
            size_t move_back = Input_System::Input::INVALID_ACTION;
            size_t move_right = Input_System::Input::INVALID_ACTION;
            size_t move_left = Input_System::Input::INVALID_ACTION;
            size_t move_up = Input_System::Input::INVALID_ACTION;
            size_t move_down = Input_System::Input::INVALID_ACTION;
            size_t sprint = Input_System::Input::INVALID_ACTION;
            size_t toggle_camera = Input_System::Input::INVALID_ACTION;
        };
        Action_Ids ids;
        bool       ids_resolved = false;

        // =========================================================
        // Internal state
        // =========================================================

        // Yaw (around world Y) and pitch (around local X), in radians:
        // cache of the Transform's rotation, see "Rotation model" above.
        float yaw = 0.0f;
        float pitch = 0.0f;

        // Rotation stored in the Transform by the last write of this
        // controller (already normalized by Set_rotation, so it compares
        // bit for bit with the stored value). Only meaningful while
        // has_written_rotation is true.
        MathLib::Quat::Quaternion last_written_rotation = MathLib::Quat::Identity();

        // False until the first write: the first Update() always derives
        // yaw/pitch from the Transform and writes the rotation, so the
        // Transform matches the yaw/pitch model from then on.
        bool has_written_rotation = false;
    };

} // namespace EngineCore
