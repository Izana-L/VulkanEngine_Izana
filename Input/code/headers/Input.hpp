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
    //   1. Window::Poll_events()  - GLFW fires callbacks -> Input queues the
    //                               key and button events in arrival order
    //   2. Input::Update()        - publishes mouse/scroll deltas and replays
    //                               the queued events in order
    //   3. Game code queries      - Is_key_down(), Get_action_value(), etc.
    //
    // Event replay: key and button levels are not written by the
    // callbacks. Update() replays the queue in order and, for every event
    // that changes a level, updates the actions bound to that key or
    // button and counts the transition. Edges are therefore counted, not
    // derived from comparing two snapshots, and a key pressed and released
    // within one Poll_events() still reports its press and its release.
    // Consequences, all by design:
    //   - Was_key_pressed() can be true while Is_key_down() is false (the
    //     key was pressed and released during the same frame); the same
    //     holds for mouse buttons and actions.
    //   - The *_press_count() queries report how many presses happened in
    //     the frame, so a toggle can decide whether two taps cancel out.
    //   - GLFW_REPEAT keeps a key held but never counts as a press.
    //   - An action bound to several keys is active while any of them is
    //     held: pressing a second binding of an active action is not a new
    //     press of the action, and releasing one of two held bindings is
    //     not a release.
    //
    // While the window is minimized the loop does not run steps 2-3; it
    // calls Discard_pending() instead, so that mouse motion and scroll
    // received during that time is dropped rather than delivered as one
    // huge delta on the first frame after restoring, and presses made
    // while nothing listens are not reported afterwards.
    //
    // Subscribes to Window via std::function callbacks (Set_*_callback).
    // Does not touch GLFW directly except for cursor mode.
    class Input
    {
    public:

        explicit Input(Platform::Window& _window);
        ~Input() ;

        Input(const Input&) = delete;
        Input& operator=(const Input&) = delete;
        Input(Input&&) = delete;
        Input& operator=(Input&&) = delete;

        // =========================================================
        // Frame update
        // =========================================================

        // Must be called once per frame AFTER Poll_events() and BEFORE
        // any game code queries input state.
        // Publishes the accumulated mouse/scroll deltas, resets the
        // accumulators and the press/release counts of the previous frame,
        // and replays the queued key/button events in arrival order,
        // updating levels, action values and this frame's counts.
        void Update();

        // Drops everything accumulated since the last Update(): mouse
        // motion, scroll, and the presses and releases of the queued
        // events. The queued events still update the levels (without
        // counting any transition), so a key released while nothing was
        // listening is not left held. Used while the window is minimized,
        // when no frame consumes the input: without it the accumulators
        // keep growing and the first frame after restoring receives the
        // whole backlog at once.
        void Discard_pending();

        // =========================================================
        // Raw keyboard state
        // =========================================================

        // True while the key is held down (level after the last event of
        // the frame).
        bool Is_key_down(Key _key) const;

        // True when the key was pressed at least once during the frame,
        // even if it was released again before Update().
        bool Was_key_pressed(Key _key) const;

        // True when the key was released at least once during the frame.
        bool Was_key_released(Key _key) const;

        // Number of presses / releases of the key during the frame.
        uint32_t Get_key_press_count(Key _key) const;
        uint32_t Get_key_release_count(Key _key) const;

        // =========================================================
        // Raw mouse state
        // =========================================================

        // Same semantics as the keyboard queries above.
        bool     Is_mouse_button_down(Mouse_Button _button) const;
        bool     Was_mouse_button_pressed(Mouse_Button _button) const;
        bool     Was_mouse_button_released(Mouse_Button _button) const;
        uint32_t Get_mouse_button_press_count(Mouse_Button _button) const;
        uint32_t Get_mouse_button_release_count(Mouse_Button _button) const;

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

        // Switching modes discards any mouse delta latched for the current
        // frame and any motion accumulated since, so a toggle never turns
        // the movement that triggered it into a camera jump.
        void        Set_cursor_mode(Cursor_Mode _mode);
        Cursor_Mode Get_cursor_mode() const;

        // =========================================================
        // Action system
        // =========================================================

        // Loads action bindings from a JSON file.
        // Format: { "actions": [ { "name": "MoveForward", "bindings": ["W", "Arrow_Up"] } ] }
        // Replaces any previously loaded actions.
        //
        // Throws std::runtime_error if the file cannot be opened, is not
        // valid JSON, or names a key or mouse button that does not exist:
        // a misspelled binding is a configuration error, and reporting it
        // at load time is what keeps it from silently binding to nothing.
        void Load_actions(const std::string& _path);

        // 0.0 (inactive) or 1.0 (active) for keyboard/mouse bindings.
        // Returns 0.0 if the action name is not found.
        float Get_action_value(const std::string& _action) const;

        // Resolves an action name to a stable index, once. Per-frame code
        // should cache the result and use the overloads below instead of
        // hashing a string every frame.
        //
        // NOTE: Load_actions() rebuilds the table, so any cached index is
        // invalid afterwards. Today it is only called at startup.
        //
        // Was_action_pressed / Was_action_released: at least one
        // transition of the action during the frame (same semantics as
        // Was_key_pressed). The counts report how many.
        static constexpr size_t INVALID_ACTION = ~size_t(0);

        size_t   Get_action_id(const std::string& _action) const;
        float    Get_action_value(size_t _action_id) const;
        bool     Is_action_down(size_t _action_id) const;
        bool     Was_action_pressed(size_t _action_id) const;
        bool     Was_action_released(size_t _action_id) const;
        uint32_t Get_action_press_count(size_t _action_id) const;
        uint32_t Get_action_release_count(size_t _action_id) const;

        // Convenience wrappers built on Get_action_value.
        bool  Is_action_down(const std::string& _action) const;
        bool  Was_action_pressed(const std::string& _action) const;
        bool  Was_action_released(const std::string& _action) const;

        // =========================================================
        // Name translation (single table, see Input.cpp)
        // =========================================================

        // Name of a key as written in the action JSON ("Arrow_Up").
        // Key::Unknown and out-of-range values yield "Unknown".
        static const char* Key_to_string(Key _key);
        static const char* Mouse_button_to_string(Mouse_Button _button);

        // Reverse lookups. Unknown names return Key::Unknown /
        // Mouse_Button::COUNT (the sentinel); Load_actions treats both as
        // errors.
        static Key          String_to_key(const std::string& _str);
        static Mouse_Button String_to_mouse_button(const std::string& _str);

    private:

        // =========================================================
        // Internal types
        // =========================================================

        static constexpr size_t KEY_COUNT = static_cast<size_t>(Key::COUNT);
        static constexpr size_t BTN_COUNT = static_cast<size_t>(Mouse_Button::COUNT);

        // Level and per-frame transition counts of every key or button of
        // one device.
        template <size_t COUNT>
        struct Button_States
        {
            std::array<bool, COUNT>     down{};
            std::array<uint32_t, COUNT> press_count{};
            std::array<uint32_t, COUNT> release_count{};

            // Actions bound to each key or button (indices into actions).
            // An action that binds the same input twice appears twice, and
            // its active_bindings moves by two, symmetrically.
            std::array<std::vector<size_t>, COUNT> bound_actions{};
        };

        // One key or button event, queued by the GLFW callbacks and
        // replayed by Update() in arrival order.
        struct Button_Event
        {
            enum class Device : uint8_t { Keyboard, Mouse };
            enum class Kind : uint8_t { Press, Release, Repeat };

            Device   device = Device::Keyboard;
            Kind     kind = Kind::Press;
            uint16_t code = 0;   // Key or Mouse_Button value
        };

        // =========================================================
        // GLFW callbacks - called by Window during Poll_events()
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

        // =========================================================
        // Action helpers
        // =========================================================

        // Returns the raw bool state (down/not) for a single binding.
        bool Binding_is_down(const Action_Binding& _binding) const;

        // Replays the queued events in order and empties the queue. Every
        // event that changes a level updates the actions bound to its key
        // or button; with _count_transitions the transitions are also
        // added to this frame's press/release counts.
        void Replay_events(bool _count_transitions);

        // Applies one level change of a key or button to the actions bound
        // to it (_bound_actions).
        void Apply_binding_transition(const std::vector<size_t>& _bound_actions, bool _down, bool _count_transitions);

        // Zeroes the press/release counts of keys, buttons and actions.
        void Clear_transition_counts();

        // Rebuilds the input -> actions maps and derives every action's
        // active_bindings and value from the current levels. Used when the
        // action table is replaced.
        void Rebind_actions();

        // Zeroes the latched deltas and the accumulators.
        void Clear_motion();

        // =========================================================
        // Data
        // =========================================================

        Platform::Window& window;

        // Keyboard and mouse state, written only by Replay_events() and
        // Rebind_actions().
        Button_States<KEY_COUNT> keys;
        Button_States<BTN_COUNT> buttons;

        // Key and button events received since the last Update() or
        // Discard_pending(), in arrival order.
        std::vector<Button_Event> pending_events;

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
        std::unordered_map<std::string, size_t>    action_index;   // name -> index in actions
    };

} // namespace Input_System
