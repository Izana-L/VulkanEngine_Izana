#include <Camera_Controller.hpp>

#include <World.hpp>
#include <Transform_Component.hpp>
#include <Input.hpp>
#include <Quaternion.hpp>
#include <Vector3.hpp>

#include <algorithm>
#include <cmath>

namespace EngineCore
{

    void Camera_Controller::Update(ECS::Entity _camera_entity,
        Input_System::Input& _input,
        ECS::World& _world,
        float         _dt)
    {
        if (!ids_resolved)
        {
            ids.move_forward = _input.Get_action_id("MoveForward");
            ids.move_back = _input.Get_action_id("MoveBack");
            ids.move_right = _input.Get_action_id("MoveRight");
            ids.move_left = _input.Get_action_id("MoveLeft");
            ids.move_up = _input.Get_action_id("MoveUp");
            ids.move_down = _input.Get_action_id("MoveDown");
            ids.sprint = _input.Get_action_id("Sprint");
            ids.toggle_camera = _input.Get_action_id("ToggleCamera");
            ids_resolved = true;
        }

        ECS::Transform_Component* transform = _world.Try_get_component<ECS::Transform_Component>(_camera_entity);

        if (!transform) return;

        // =========================================================
        // Toggle cursor capture
        // =========================================================

        // ToggleCamera switches between captured (Camera) and free (Window)
        // cursor. Rotation is only applied while captured. Set_cursor_mode
        // discards the mouse delta of the frame it runs in, so the click
        // that toggles never becomes a rotation.
        if (_input.Was_action_pressed(ids.toggle_camera))
        {
            const Input_System::Cursor_Mode new_mode =
                (_input.Get_cursor_mode() == Input_System::Cursor_Mode::Camera)
                ? Input_System::Cursor_Mode::Window
                : Input_System::Cursor_Mode::Camera;
            _input.Set_cursor_mode(new_mode);
        }

        // =========================================================
        // Initialize yaw/pitch from current rotation (first frame)
        // =========================================================

        if (!initialized)
        {
            // Derived from the rotated basis vectors rather than from an
            // Euler decomposition. glm::eulerAngles returns an equivalent
            // (pitch = pi, yaw' = pi - yaw, roll = pi) decomposition for
            // |yaw| > 90 degrees; reading only its x and y and dropping
            // the roll rebuilds a different rotation, and the camera snaps
            // on the first frame. The basis vectors have no such ambiguity:
            //   forward = (-cos(pitch) sin(yaw), sin(pitch), -cos(pitch) cos(yaw))
            //   right   = ( cos(yaw), 0, -sin(yaw))
            // for the yaw * pitch model rebuilt below, so pitch comes from
            // forward.y and yaw from the right vector, which stays well
            // defined even when the camera looks straight up or down.
            const MathLib::Vector3 forward = MathLib::Quat::GetForward(transform->rotation);
            const MathLib::Vector3 right = MathLib::Quat::GetRight(transform->rotation);

            pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
            yaw = std::atan2(-right.z, right.x);

            pitch = std::clamp(pitch, -pitch_limit, pitch_limit);

            initialized = true;
        }

        // =========================================================
        // Rotation from mouse (only in Camera cursor mode)
        // =========================================================

        if (_input.Get_cursor_mode() == Input_System::Cursor_Mode::Camera)
        {
            const float dx = _input.Get_mouse_delta_x();
            const float dy = _input.Get_mouse_delta_y();

            // Horizontal mouse -> yaw (negative so right-drag looks right).
            yaw -= dx * mouse_sensitivity;

            // Vertical mouse -> pitch (negative so up-drag looks up).
            pitch -= dy * mouse_sensitivity;

            // Clamp pitch to avoid flipping over the poles.
            pitch = std::clamp(pitch, -pitch_limit, pitch_limit);
        }

        // Rebuild the rotation quaternion from yaw and pitch every frame
        // (also on the first one, so the transform matches the projected
        // yaw/pitch state from then on).
        // Order: yaw around world Y, then pitch around local X.
        const MathLib::Quat::Quaternion q_yaw =
            MathLib::Quat::From_axis_angle(MathLib::Vector3(0.0f, 1.0f, 0.0f), yaw);

        const MathLib::Quat::Quaternion q_pitch =
            MathLib::Quat::From_axis_angle(MathLib::Vector3(1.0f, 0.0f, 0.0f), pitch);

        // yaw * pitch: pitch applied first (local), then yaw (world).
        transform->Set_rotation(MathLib::Quat::Multiply(q_yaw, q_pitch));

        // =========================================================
        // Movement from keyboard
        // =========================================================

        // Build a movement vector from the action values.
        // Forward/Right come from the camera's current orientation;
        // Up/Down use the Y axis (typical free-flight behavior).
        const MathLib::Vector3 forward = transform->Forward();
        const MathLib::Vector3 right = transform->Right();
        const MathLib::Vector3 world_up(0.0f, 1.0f, 0.0f);

        MathLib::Vector3 movement(0.0f, 0.0f, 0.0f);

        movement += forward * _input.Get_action_value(ids.move_forward);
        movement -= forward * _input.Get_action_value(ids.move_back);
        movement += right * _input.Get_action_value(ids.move_right);
        movement -= right * _input.Get_action_value(ids.move_left);
        movement += world_up * _input.Get_action_value(ids.move_up);
        movement -= world_up * _input.Get_action_value(ids.move_down);

        // Normalize so diagonal movement isn't faster than axis-aligned.
        const float length_sq = MathLib::Vec3::Length_squared(movement);
        if (length_sq > 1e-6f)
        {
            movement = MathLib::Vec3::Normalize(movement);

            float speed = move_speed;
            if (_input.Is_action_down(ids.sprint))
                speed *= sprint_multiplier;

            transform->Set_position(transform->position + movement * speed * _dt);
        }
    }

} // namespace EngineCore
