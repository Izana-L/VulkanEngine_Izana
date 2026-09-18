#pragma once

#include <Key.hpp>
#include <Input_Action.hpp>
#include <Window.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Input_System
{

    // Input: manages keyboard and mouse state for one frame, and resolves
    // named actions loaded from a JSON configuration file.
    //
    // Lifecycle per frame:
    //   1. Window::Poll_events()  — GLFW fires callbacks → Input stores raw events
    //   2. Input::Update()        — swap snapshots, compute action states
    //   3. Game code queries      — Is_key_down(), Get_action_value(), etc.
    //
    // Subscribes to Window via std::function callbacks (Set_*_callback).
    // Does not touch GLFW directly except for cursor mode.
    class Input
    {
    public:

        explicit Input(Platform::Window& _window);
        ~Input() = default;

        Input(const Input&) = delete;
        Input& operator=(const Input&) = delete;
        Input(Input&&) = delete;
        Input& operator=(Input&&) = delete;

        // =========================================================
        // Frame update
        // =========================================================

        // Must be called once per frame AFTER Poll_events() and BEFORE
        // any game code queries input state.
        // Swaps current/previous snapshots and resets per-frame data
        // (mouse delta, scroll, pressed/released flags).
        void Update();

        // =========================================================
        // Raw keyboard state
        // =========================================================

        // True while the key is held down.
        bool Is_key_down(Key _key) const;

        // True only on the frame the key transitioned from up to down.
        bool Was_key_pressed(Key _key) const;

        // True only on the frame the key transitioned from down to up.
        bool Was_key_released(Key _key) const;

        // =========================================================
        // Raw mouse state
        // =========================================================

        bool Is_mouse_button_down(Mouse_Button _button) const;
        bool Was_mouse_button_pressed(Mouse_Button _button) const;
        bool Was_mouse_button_released(Mouse_Button _button) const;

        // Mouse movement since last frame.
        // Meaningful in Camera mode (cursor captured).
        // In Window mode returns the delta between two absolute positions.
        float Get_mouse_delta_x() const;
        float Get_mouse_delta_y() const;

        // Absolute cursor position in screen coordinates.
        // Meaningful in Window mode.
        float Get_mouse_x() const;
        float Get_mouse_y() const;

        // Scroll wheel delta since last frame (positive = up).
        float Get_scroll_delta() const;

        // =========================================================
        // Cursor mode
        // =========================================================

        void        Set_cursor_mode(Cursor_Mode _mode);
        Cursor_Mode Get_cursor_mode() const;

        // =========================================================
        // Action system
        // =========================================================

        // Loads action bindings from a JSON file.
        // Format: { "actions": [ { "name": "MoveForward", "bindings": ["W", "Arrow_Up"] } ] }
        // Replaces any previously loaded actions.
        void Load_actions(const std::string& _path);

        // 0.0 (inactive) or 1.0 (active) for keyboard/mouse bindings.
        // Returns 0.0 if the action name is not found.
        float Get_action_value(const std::string& _action) const;

        // Convenience wrappers built on Get_action_value.
        bool  Is_action_down(const std::string& _action) const;
        bool  Was_action_pressed(const std::string& _action) const;
        bool  Was_action_released(const std::string& _action) const;

    private:

        // =========================================================
        // Internal types
        // =========================================================

        static constexpr size_t KEY_COUNT = static_cast<size_t>(Key::COUNT);
        static constexpr size_t BTN_COUNT = static_cast<size_t>(Mouse_Button::COUNT);

        struct Keyboard_State
        {
            std::array<bool, KEY_COUNT> keys{};
        };

        struct Mouse_State
        {
            std::array<bool, BTN_COUNT> buttons{};
        };

        // =========================================================
        // GLFW callbacks — called by Window during Poll_events()
        // =========================================================

        void Handle_key(int _glfw_key, int _glfw_action);
        void Handle_mouse_button(int _glfw_button, int _glfw_action);
        void Handle_mouse_move(double _xpos, double _ypos);
        void Handle_scroll(double _yoffset);

        // =========================================================
        // Key translation
        // =========================================================

        static Key          Glfw_key_to_key(int _glfw_key);
        static Mouse_Button Glfw_button_to_btn(int _glfw_button);
        static std::string  Key_to_string(Key _key);
        static Key          String_to_key(const std::string& _str);
        static Mouse_Button String_to_mouse_button(const std::string& _str);

        // =========================================================
        // Action helpers
        // =========================================================

        // Returns the raw bool state (down/not) for a single binding.
        bool Binding_is_down(const Action_Binding& _binding) const;

        // Recomputes value, pressed and released for all actions.
        void Update_actions();

        // =========================================================
        // Data
        // =========================================================

        Platform::Window& window;

        // Double-buffered keyboard and mouse state.
        Keyboard_State current_keys;
        Keyboard_State previous_keys;
        Mouse_State    current_buttons;
        Mouse_State    previous_buttons;

        // Mouse position and delta.
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        float mouse_delta_x = 0.0f;   // exposed to callers via Get_mouse_delta_x()
        float mouse_delta_y = 0.0f;   // exposed to callers via Get_mouse_delta_y()
        float mouse_delta_x_acc = 0.0f; // accumulates during Poll_events()
        float mouse_delta_y_acc = 0.0f; // accumulates during Poll_events()
        bool  first_mouse = true;

        float scroll_delta = 0.0f;
        float scroll_accumulator = 0.0f;  // accumulate during Poll_events()

        Cursor_Mode cursor_mode = Cursor_Mode::Window;

        // Actions loaded from JSON.
        std::vector<Action>                        actions;
        std::unordered_map<std::string, size_t>    action_index;   // name → index in actions
    };

} // namespace Input