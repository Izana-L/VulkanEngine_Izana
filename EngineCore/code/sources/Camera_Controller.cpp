#include <Camera_Controller.hpp>

#include <World.hpp>
#include <Transform_Component.hpp>
#include <Input.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace EngineCore
{

    void Camera_Controller::Update(CoreTypes::Id _camera_entity,
        Input_System::Input& _input,
        ECS::World& _world,
        float         _dt)
    {
        ECS::Transform_Component* transform =
            _world.Try_get_component<ECS::Transform_Component>(_camera_entity);
        if (!transform) return;

        // =========================================================
        // Toggle cursor capture
        // =========================================================

        // ToggleCamera switches between captured (Camera) and free (Window)
        // cursor. Rotation is only applied while captured.
        if (_input.Was_action_pressed("ToggleCamera"))
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
            // Extract yaw/pitch from the current rotation so the camera
            // doesn't snap to (0,0) on the first update.
            const MathLib::Vector3 euler =
                MathLib::Quat::ToEuler(transform->rotation);
            pitch = euler.x;
            yaw = euler.y;
            initialized = true;
        }

        // =========================================================
        // Rotation from mouse (only in Camera cursor mode)
        // =========================================================

        if (_input.Get_cursor_mode() == Input_System::Cursor_Mode::Camera)
        {
            const float dx = _input.Get_mouse_delta_x();
            const float dy = _input.Get_mouse_delta_y();

            // Horizontal mouse → yaw (negative so right-drag looks right).
            yaw -= dx * mouse_sensitivity;

            // Vertical mouse → pitch (negative so up-drag looks up).
            pitch -= dy * mouse_sensitivity;

            // Clamp pitch to avoid flipping over the poles.
            pitch = std::clamp(pitch, -pitch_limit, pitch_limit);

            // Rebuild the rotation quaternion from yaw and pitch.
            // Order: yaw around world Y, then pitch around local X.
            const MathLib::Quat::Quaternion q_yaw =
                MathLib::Quat::From_axis_angle(
                    MathLib::Vector3(0.0f, 1.0f, 0.0f), yaw);

            const MathLib::Quat::Quaternion q_pitch =
                MathLib::Quat::From_axis_angle(
                    MathLib::Vector3(1.0f, 0.0f, 0.0f), pitch);

            // yaw * pitch: pitch applied first (local), then yaw (world).
            transform->Set_rotation(
                MathLib::Quat::Multiply(q_yaw, q_pitch));
        }

        // =========================================================
        // Movement from keyboard
        // =========================================================

        // Build a movement vector in world space from the action values.
        // Forward/Right come from the camera's current orientation;
        // Up/Down use the world Y axis (typical free-flight behavior).
        const MathLib::Vector3 forward = transform->Forward();
        const MathLib::Vector3 right = transform->Right();
        const MathLib::Vector3 world_up(0.0f, 1.0f, 0.0f);

        MathLib::Vector3 movement(0.0f, 0.0f, 0.0f);

        movement += forward * _input.Get_action_value("MoveForward");
        movement -= forward * _input.Get_action_value("MoveBack");
        movement += right * _input.Get_action_value("MoveRight");
        movement -= right * _input.Get_action_value("MoveLeft");
        movement += world_up * _input.Get_action_value("MoveUp");
        movement -= world_up * _input.Get_action_value("MoveDown");

        // Normalize so diagonal movement isn't faster than axis-aligned.
        const float length_sq = glm::dot(movement, movement);
        if (length_sq > 1e-6f)
        {
            movement = glm::normalize(movement);

            float speed = move_speed;
            if (_input.Is_action_down("Sprint"))
                speed *= sprint_multiplier;

            const MathLib::Vector3 new_position =
                transform->position + movement * speed * _dt;

            transform->Set_position(new_position);
        }
    }

} // namespace EngineCore