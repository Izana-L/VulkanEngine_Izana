#include <Camera_Controller.hpp>
#include <Camera_Component.hpp>
#include <Engine_Context.hpp>
#include <World.hpp>
#include <Transform_Component.hpp>
#include <Input.hpp>
#include <Quaternion.hpp>
#include <Vector3.hpp>

#include <iostream>
#include <algorithm>
#include <cmath>

namespace Game
{

    void Camera_Controller::Bind_actions(const Input_System::Input& _input)
    {
        constexpr const char* CONSUMER = "Camera_Controller";

        ids.move_forward = _input.Resolve_action_id("MoveForward", CONSUMER);
        ids.move_back = _input.Resolve_action_id("MoveBack", CONSUMER);
        ids.move_right = _input.Resolve_action_id("MoveRight", CONSUMER);
        ids.move_left = _input.Resolve_action_id("MoveLeft", CONSUMER);
        ids.move_up = _input.Resolve_action_id("MoveUp", CONSUMER);
        ids.move_down = _input.Resolve_action_id("MoveDown", CONSUMER);
        ids.sprint = _input.Resolve_action_id("Sprint", CONSUMER);
        ids.toggle_camera = _input.Resolve_action_id("ToggleCamera", CONSUMER);
    }
    void Camera_Controller::Create_camera(ECS::World& _world, const MathLib::Vector3& _position)
    {
        camera_entity = _world.Create_entity();

        _world.Add_component<ECS::Transform_Component>(camera_entity).Set_position(_position);
        _world.Add_component<ECS::Camera_Component>(camera_entity, ECS::Camera_Component::Make_perspective());

        std::cout << "[Camera_Controller] Camera entity created (id=" << camera_entity << ").\n";
    }
    void Camera_Controller::Update(EngineCore::Engine_Context& _context, float _dt)
    {
        Input_System::Input& input = _context.Input();

        ECS::Transform_Component* transform = _context.World().Try_get_component<ECS::Transform_Component>(camera_entity);

        if (!transform) return;

        // =========================================================
        // Toggle cursor capture
        // =========================================================

        // ToggleCamera switches between captured (Camera) and free (Window)
        // cursor. Rotation is only applied while captured. Set_cursor_mode
        // discards the mouse delta of the frame it runs in, so the click
        // that toggles never becomes a rotation.
        if (input.Was_action_pressed(ids.toggle_camera))
        {
            const Input_System::Cursor_Mode new_mode =
                (input.Get_cursor_mode() == Input_System::Cursor_Mode::Camera)
                ? Input_System::Cursor_Mode::Window
                : Input_System::Cursor_Mode::Camera;
            input.Set_cursor_mode(new_mode);
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

        if (input.Get_cursor_mode() == Input_System::Cursor_Mode::Camera)
        {
            const float dx = input.Get_mouse_delta_x();
            const float dy = input.Get_mouse_delta_y();

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

        movement += forward * input.Get_action_value(ids.move_forward);
        movement -= forward * input.Get_action_value(ids.move_back);
        movement += right * input.Get_action_value(ids.move_right);
        movement -= right * input.Get_action_value(ids.move_left);
        movement += world_up * input.Get_action_value(ids.move_up);
        movement -= world_up * input.Get_action_value(ids.move_down);

        // Normalize so diagonal movement isn't faster than axis-aligned.
        const float length_sq = MathLib::Vec3::Length_squared(movement);
        if (length_sq > 1e-6f)
        {
            movement = MathLib::Vec3::Normalize(movement);

            float speed = move_speed;
            if (input.Is_action_down(ids.sprint))
                speed *= sprint_multiplier;

            transform->Set_position(transform->position + movement * speed * _dt);
        }
    }

} // namespace EngineCore