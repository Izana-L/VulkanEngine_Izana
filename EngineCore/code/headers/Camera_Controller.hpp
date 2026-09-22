#pragma once

#include <Entity.hpp>
#include <Input.hpp>

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
    // Rotation model: the controller keeps its own yaw and pitch as
    // internal state, updates them from the mouse delta each frame, and
    // rebuilds the Transform's rotation quaternion from scratch. This
    // avoids drift that would accumulate if it read and re-applied the
    // existing rotation. Roll is not supported: a rolled initial rotation
    // is projected onto the yaw/pitch model on the first update.
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

        // Accumulated yaw (around world Y) and pitch (around local X),
        // in radians. Source of truth for the camera's orientation.
        float yaw = 0.0f;
        float pitch = 0.0f;

        // First Update() initializes yaw/pitch from the entity's current
        // rotation so the camera doesn't snap on the first frame.
        bool  initialized = false;
    };

} // namespace EngineCore
