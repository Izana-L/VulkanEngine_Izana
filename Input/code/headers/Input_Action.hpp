#pragma once

#include <Key.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Input
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
            Input::Key          key;
            Input::Mouse_Button mouse_button;
        };

        // Convenience constructors.
        static Action_Binding From_key(Input::Key _key)
        {
            Action_Binding b;
            b.type = Type::Key;
            b.key = _key;
            return b;
        }

        static Action_Binding From_mouse_button(Input::Mouse_Button _button)
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
    // value:    0.0 (inactive) or 1.0 (active). Always 0.0 or 1.0 for
    //           keyboard/mouse; reserved for analogue range when gamepad
    //           support is added.
    // pressed:  true only on the frame the action became active.
    // released: true only on the frame the action became inactive.
    struct Action
    {
        std::string                  name;
        std::vector<Action_Binding>  bindings;

        // Per-frame state — written by Action_Map::Update(), read by callers.
        float value = 0.0f;
        bool  pressed = false;   // transition inactive → active this frame
        bool  released = false;   // transition active → inactive this frame
    };

} // namespace Input