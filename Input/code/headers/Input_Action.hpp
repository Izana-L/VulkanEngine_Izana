#pragma once

#include <Key.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Input_System
{

    // Action_Binding: a single input source that can trigger an action.
    // Each binding is either a keyboard key or a mouse button.
    // Multiple bindings per action work as OR — any one of them
    // being active makes the action active.
    struct Action_Binding
    {
        enum class Type : uint8_t
        {
            Key,          // keyboard key
            Mouse_Button  // mouse button
        };

        Type type = Type::Key;

        union
        {
            Key          key = Key::Unknown;
            Mouse_Button mouse_button;
        };

        // Convenience constructors.
        static Action_Binding From_key(Key _key)
        {
            Action_Binding b;
            b.type = Type::Key;
            b.key = _key;
            return b;
        }

        static Action_Binding From_mouse_button(Mouse_Button _button)
        {
            Action_Binding b;
            b.type = Type::Mouse_Button;
            b.mouse_button = _button;
            return b;
        }
    };

    // Action: a named logical input that can be triggered by one or
    // more bindings. The game code queries actions by name, never by
    // raw key — this is what allows remapping via the JSON file.
    //
    // value:           0.0 (inactive) or 1.0 (active). Always 0.0 or 1.0
    //                  for keyboard/mouse; reserved for analogue range when
    //                  gamepad support is added.
    // active_bindings: number of bindings currently held. The action is
    //                  active while it is above zero, so holding a second
    //                  binding of an already active action is not a new
    //                  activation (OR semantics).
    // press_count:     inactive -> active transitions during the last
    //                  processed frame. Several when the action was
    //                  pressed and released more than once in one frame.
    // release_count:   active -> inactive transitions during the last
    //                  processed frame.
    struct Action
    {
        std::string                  name;
        std::vector<Action_Binding>  bindings;

        // State written by Input while it replays the input events of a
        // frame (Input::Update), read by callers through Input.
        float    value = 0.0f;
        uint32_t active_bindings = 0;
        uint32_t press_count = 0;
        uint32_t release_count = 0;
    };

} 