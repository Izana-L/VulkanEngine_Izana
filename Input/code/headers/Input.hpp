#pragma once

#include <Key.hpp>
#include <Input_Action.hpp>
#include <Window.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Input_System
{

    // Input: manages keyboard and mouse state for one frame, and resolves
    // named actions loaded from a JSON configuration file.
    //
    // Lifecycle per frame:
    //   1. Window::Poll_events()  - GLFW fires callbacks -> Input accumulates
    //                               raw events (key/button edges, mouse
    //                               motion, scroll)
    //   2. Input::Update()        - publish what was accumulated, compute
    //                               action states
    //   3. Game code queries      - Is_key_down(), Get_action_value(), etc.
    //
    // Everything that is "since the last frame" (pressed/released edges,
    // mouse delta, scroll) is accumulated by the callbacks and published
    // by Update(), so it is delivered exactly once, no matter how many
    // times Poll_events() ran in between and even if a press and its
    // release arrive within the same Poll_events().
    //
    // While the window is minimized the loop does not run steps 2-3; it
    // calls Discard_pending() instead, so that what was accumulated
    // during that time is dropped rather than delivered as one huge delta
    // (or a burst of stale edges) on the first frame after restoring.
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
        // Publishes the key/button edges and the mouse/scroll deltas
        // accumulated since the previous call, resets the accumulators,
        // and recomputes action values and edges.
        void Update();

        // Drops everything accumulated since the last Update() (mouse
        // motion, scroll, pressed/released edges). Used while the window
        // is minimized, when no frame consumes the input: without it the
        // accumulators keep growing and the first frame after restoring
        // receives the whole backlog at once. Keys that are still held
        // stay held; only the history is dropped.
        void Discard_pending();

        // =========================================================
        // Raw keyboard state
        // =========================================================
        //
        // Every query is safe for any value of the enum: Key::COUNT, or a
        // value cast from garbage, is "not a key" and reads as false
        // instead of indexing past the state arrays.

        // True while the key is held down.
        bool Is_key_down(Key _key) const;

        // True on the frame the key went down. Latched: a key that was
        // pressed AND released between two Update() calls still reports
        // true here (and in Was_key_released), even though Is_key_down()
        // is already false by then.
        bool Was_key_pressed(Key _key) const;

        // True on the frame the key went up. Same latching as above.
        bool Was_key_released(Key _key) const;

        // =========================================================
        // Raw mouse state
        // =========================================================
        //
        // Same guarantees as the keyboard: Mouse_Button::COUNT (the
        // "unknown button" sentinel) and out-of-range values read as
        // false, and a click shorter than a frame is not lost.

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

        // Switching modes discards any mouse delta latched for the current
        // frame and any motion accumulated since, so a toggle never turns
        // the movement that triggered it into a camera jump. Scroll is
        // not mouse motion and is left alone: a wheel notch that arrived
        // with the toggle is still delivered.
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
        //
        // The by-name queries (this one and the Is_/Was_ wrappers below)
        // treat an action that is not defined as inactive, but they do not
        // do it silently: the first query for each unknown name is reported
        // once on std::cerr (not every frame), so a typo in the game code,
        // or an action removed from the input JSON, shows up in the log
        // instead of passing for "the player is not pressing it".
        float Get_action_value(const std::string& _action) const;

        // Resolves an action name to a stable index, once. Per-frame code
        // should cache the result and use the overloads below instead of
        // hashing a string every frame.
        //
        // NOTE: Load_actions() rebuilds the table, so any cached index is
        // invalid afterwards. Today it is only called at startup.
        static constexpr size_t INVALID_ACTION = ~size_t(0);

        // Pure lookup: returns INVALID_ACTION for an unknown name without
        // reporting anything, so it can be used to probe for an action.
        size_t Get_action_id(const std::string& _action) const;

        // Get_action_id() for a consumer that binds its actions by name once
        // (_consumer names it in the report). An unknown name still returns
        // INVALID_ACTION, whose queries are always inactive, but it is
        // reported on std::cerr: a typo, or an action removed from the input
        // JSON, would otherwise disable its key without any message.
        size_t Resolve_action_id(const std::string& _action, const char* _consumer) const;

        float  Get_action_value(size_t _action_id) const;
        bool   Is_action_down(size_t _action_id) const;
        bool   Was_action_pressed(size_t _action_id) const;
        bool   Was_action_released(size_t _action_id) const;

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

        // State of a set of digital inputs (keys, or mouse buttons).
        //
        // Edges are latched by the callbacks and published by Update(), the
        // same accumulate / publish / reset pattern as the mouse delta. They
        // are NOT derived by comparing the level before and after
        // Poll_events(): that comparison cannot see a press that is
        // released again within the same poll (a fast tap, or a low frame
        // rate), which would make the key/button click vanish.
        //
        // All queries are range checked here, in one place, so no caller
        // can index past the arrays with an out-of-range enum value (such as
        // the COUNT sentinel).
        template <size_t COUNT>
        struct Digital_State
        {
            std::array<bool, COUNT> down{};          // live level, written by the callbacks
            std::array<bool, COUNT> pressed_acc{};   // edges seen since the last Publish()
            std::array<bool, COUNT> released_acc{};
            std::array<bool, COUNT> pressed{};       // edges published for the current frame
            std::array<bool, COUNT> released{};

            bool Is_down(size_t _index)      const { return _index < COUNT && down[_index]; }
            bool Was_pressed(size_t _index)  const { return _index < COUNT && pressed[_index]; }
            bool Was_released(size_t _index) const { return _index < COUNT && released[_index]; }

            // Records a level report from the OS. Only a real change is an
            // edge: key repeat (down -> down) and duplicate releases are not.
            void Set(size_t _index, bool _is_down)
            {
                if (_index >= COUNT || down[_index] == _is_down) return;

                down[_index] = _is_down;
                (_is_down ? pressed_acc : released_acc)[_index] = true;
            }

            // Makes the accumulated edges the current frame's and starts
            // accumulating anew.
            void Publish()
            {
                pressed = pressed_acc;
                released = released_acc;
                pressed_acc.fill(false);
                released_acc.fill(false);
            }

            // Drops every edge, published or not. The level is kept.
            void Discard_edges()
            {
                pressed_acc.fill(false);
                released_acc.fill(false);
                pressed.fill(false);
                released.fill(false);
            }
        };

        using Keyboard_State = Digital_State<KEY_COUNT>;
        using Mouse_State    = Digital_State<BTN_COUNT>;

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

        // True if the binding's key/button went down since the last
        // Update(), even when it is already up again.
        bool Binding_was_pressed(const Action_Binding& _binding) const;

        // Recomputes value, pressed and released for all actions.
        void Update_actions();

        // By-name lookup for the per-frame queries: like Get_action_id(),
        // but an unknown name is reported once (see Get_action_value).
        // _query names the calling query in the report.
        size_t Find_action_for_query(const std::string& _action, const char* _query) const;

        // Zeroes the mouse delta (published and accumulated). Scroll is a
        // separate channel with its own reset.
        void Clear_mouse_motion();

        // Zeroes the scroll delta (published and accumulated).
        void Clear_scroll();

        // =========================================================
        // Data
        // =========================================================

        Platform::Window& window;

        // Keyboard and mouse button state: live level + latched edges.
        Keyboard_State keyboard;
        Mouse_State    mouse_buttons;

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

        // Unknown names already reported by the by-name queries, so each
        // one is logged once instead of every frame. Cleared by
        // Load_actions(), after which the set of known names has changed.
        mutable std::unordered_set<std::string>    reported_unknown_actions;
    };

} // namespace Input_System
