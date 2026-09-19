#include <Input.hpp>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <json.hpp>

#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace Input_System
{

    // =========================================================
    // Constructor
    // =========================================================

    Input::Input(Platform::Window& _window): window(_window)  
    {
        // Subscribe to Window's input callbacks.
        _window.Set_key_callback([this](int key, int action)
            {
                Handle_key(key, action);
            });

        _window.Set_mouse_button_callback([this](int button, int action)
            {
                Handle_mouse_button(button, action);
            });

        _window.Set_mouse_move_callback([this](double xpos, double ypos)
            {
                Handle_mouse_move(xpos, ypos);
            });

        _window.Set_scroll_callback([this](double yoffset)
            {
                Handle_scroll(yoffset);
            });
    }

    

    void Input::Begin_frame()
    {
       
        previous_keys = current_keys;
        previous_buttons = current_buttons;
    }

    // =========================================================
    // Update — call once per frame after Poll_events()
    // =========================================================

    void Input::Update()
    {
        // Transfer accumulated mouse delta to the readable fields,
        // then reset the accumulators for the next frame.
        // This mirrors the scroll pattern: accumulate during Poll_events(),
        // expose via Update(), reset for next frame.
        mouse_delta_x = mouse_delta_x_acc;
        mouse_delta_y = mouse_delta_y_acc;
        mouse_delta_x_acc = 0.0f;
        mouse_delta_y_acc = 0.0f;

        scroll_delta = scroll_accumulator;
        scroll_accumulator = 0.0f;

        // Recompute action states based on the new snapshots.
        Update_actions();
    }

    // =========================================================
    // Raw keyboard
    // =========================================================

    bool Input::Is_key_down(Key _key) const
    {
        return current_keys.keys[static_cast<size_t>(_key)];
    }

    bool Input::Was_key_pressed(Key _key) const
    {
        const size_t idx = static_cast<size_t>(_key);
        return current_keys.keys[idx] && !previous_keys.keys[idx];
    }

    bool Input::Was_key_released(Key _key) const
    {
        const size_t idx = static_cast<size_t>(_key);
        return !current_keys.keys[idx] && previous_keys.keys[idx];
    }

    // =========================================================
    // Raw mouse
    // =========================================================

    bool Input::Is_mouse_button_down(Mouse_Button _button) const
    {
        return current_buttons.buttons[static_cast<size_t>(_button)];
    }

    bool Input::Was_mouse_button_pressed(Mouse_Button _button) const
    {
        const size_t idx = static_cast<size_t>(_button);
        return current_buttons.buttons[idx] && !previous_buttons.buttons[idx];
    }

    bool Input::Was_mouse_button_released(Mouse_Button _button) const
    {
        const size_t idx = static_cast<size_t>(_button);
        return !current_buttons.buttons[idx] && previous_buttons.buttons[idx];
    }

    float Input::Get_mouse_delta_x() const { return mouse_delta_x; }
    float Input::Get_mouse_delta_y() const { return mouse_delta_y; }
    float Input::Get_mouse_x()       const { return mouse_x; }
    float Input::Get_mouse_y()       const { return mouse_y; }
    float Input::Get_scroll_delta()  const { return scroll_delta; }

    // =========================================================
    // Cursor mode
    // =========================================================

    void Input::Set_cursor_mode(Cursor_Mode _mode)
    {
        if (_mode == cursor_mode) return;

        cursor_mode = _mode;
        first_mouse = true;     // avoid a jump on mode switch

        const int glfw_mode = (_mode == Cursor_Mode::Camera)
            ? GLFW_CURSOR_DISABLED
            : GLFW_CURSOR_NORMAL;

        glfwSetInputMode(window.Get_native_handle(), GLFW_CURSOR, glfw_mode);

        // Enable raw mouse motion in camera mode if the driver supports it.
        // Raw motion bypasses OS pointer acceleration for more predictable
        // camera rotation.
        if (_mode == Cursor_Mode::Camera &&
            glfwRawMouseMotionSupported())
        {
            glfwSetInputMode(window.Get_native_handle(),
                GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
        else
        {
            glfwSetInputMode(window.Get_native_handle(),
                GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        }
    }

    Cursor_Mode Input::Get_cursor_mode() const
    {
        return cursor_mode;
    }

    // =========================================================
    // Action system
    // =========================================================

    void Input::Load_actions(const std::string& _path)
    {
        std::ifstream file(_path);
        if (!file.is_open())
            throw std::runtime_error("Input: cannot open action file '" + _path + "'");

        nlohmann::json json;
        file >> json;

        actions.clear();
        action_index.clear();

        for (const auto& entry : json.at("actions"))
        {
            Action action;
            action.name = entry.at("name").get<std::string>();

            for (const auto& binding_str : entry.at("bindings"))
            {
                const std::string str = binding_str.get<std::string>();

                // Mouse buttons are prefixed with "Mouse_".
                if (str.rfind("Mouse_", 0) == 0 ||
                    str == "Left_Mouse" ||
                    str == "Right_Mouse" ||
                    str == "Middle_Mouse")
                {
                    action.bindings.push_back(
                        Action_Binding::From_mouse_button(String_to_mouse_button(str)));
                }
                else
                {
                    action.bindings.push_back(
                        Action_Binding::From_key(String_to_key(str)));
                }
            }

            const size_t idx = actions.size();
            action_index[action.name] = idx;
            actions.push_back(std::move(action));
        }

        std::cout << "[Input] Loaded " << actions.size()
            << " action(s) from '" << _path << "'\n";
    }

    float Input::Get_action_value(const std::string& _action) const
    {
        auto it = action_index.find(_action);
        if (it == action_index.end()) return 0.0f;
        return actions[it->second].value;
    }

    bool Input::Is_action_down(const std::string& _action) const
    {
        return Get_action_value(_action) > 0.5f;
    }

    bool Input::Was_action_pressed(const std::string& _action) const
    {
        auto it = action_index.find(_action);
        if (it == action_index.end()) return false;
        return actions[it->second].pressed;
    }

    bool Input::Was_action_released(const std::string& _action) const
    {
        auto it = action_index.find(_action);
        if (it == action_index.end()) return false;
        return actions[it->second].released;
    }

    // =========================================================
    // GLFW event handlers
    // =========================================================

    void Input::Handle_key(int _glfw_key, int _glfw_action)
    {
        const Key key = Glfw_key_to_key(_glfw_key);
        if (key == Key::Unknown) return;

        const size_t idx = static_cast<size_t>(key);

      
        current_keys.keys[idx] = (_glfw_action != GLFW_RELEASE);
    }

    void Input::Handle_mouse_button(int _glfw_button, int _glfw_action)
    {
        const Mouse_Button btn = Glfw_button_to_btn(_glfw_button);
        if (static_cast<size_t>(btn) >= BTN_COUNT) return;

        current_buttons.buttons[static_cast<size_t>(btn)] =
            (_glfw_action != GLFW_RELEASE);
    }

    void Input::Handle_mouse_move(double _xpos, double _ypos)
    {
        const float fx = static_cast<float>(_xpos);
        const float fy = static_cast<float>(_ypos);

        if (first_mouse)
        {
            mouse_x = fx;
            mouse_y = fy;
            first_mouse = false;
            return;
        }

        // Accumulate delta into separate accumulators.
        // Update() transfers them to mouse_delta_x/y each frame.
        mouse_delta_x_acc += fx - mouse_x;
        mouse_delta_y_acc += fy - mouse_y;
        mouse_x = fx;
        mouse_y = fy;
    }

    void Input::Handle_scroll(double _yoffset)
    {
        scroll_accumulator += static_cast<float>(_yoffset);
    }

    // =========================================================
    // Update_actions
    // =========================================================

    void Input::Update_actions()
    {
        for (Action& action : actions)
        {
            const float prev_value = action.value;

            // OR of all bindings: any active binding makes the action active.
            float new_value = 0.0f;
            for (const Action_Binding& binding : action.bindings)
            {
                if (Binding_is_down(binding))
                {
                    new_value = 1.0f;
                    break;
                }
            }

            action.value = new_value;
            action.pressed = (new_value > 0.5f) && (prev_value < 0.5f);
            action.released = (new_value < 0.5f) && (prev_value > 0.5f);
        }
    }

    bool Input::Binding_is_down(const Action_Binding& _binding) const
    {
        if (_binding.type == Action_Binding::Type::Key)
            return Is_key_down(_binding.key);
        else
            return Is_mouse_button_down(_binding.mouse_button);
    }

    // =========================================================
    // GLFW key translation
    // =========================================================

    Key Input::Glfw_key_to_key(int _glfw_key)
    {
        switch (_glfw_key)
        {
        case GLFW_KEY_SPACE:         return Key::Space;
        case GLFW_KEY_APOSTROPHE:    return Key::Apostrophe;
        case GLFW_KEY_COMMA:         return Key::Comma;
        case GLFW_KEY_MINUS:         return Key::Minus;
        case GLFW_KEY_PERIOD:        return Key::Period;
        case GLFW_KEY_SLASH:         return Key::Slash;
        case GLFW_KEY_0:             return Key::Num_0;
        case GLFW_KEY_1:             return Key::Num_1;
        case GLFW_KEY_2:             return Key::Num_2;
        case GLFW_KEY_3:             return Key::Num_3;
        case GLFW_KEY_4:             return Key::Num_4;
        case GLFW_KEY_5:             return Key::Num_5;
        case GLFW_KEY_6:             return Key::Num_6;
        case GLFW_KEY_7:             return Key::Num_7;
        case GLFW_KEY_8:             return Key::Num_8;
        case GLFW_KEY_9:             return Key::Num_9;
        case GLFW_KEY_SEMICOLON:     return Key::Semicolon;
        case GLFW_KEY_EQUAL:         return Key::Equal;
        case GLFW_KEY_A:             return Key::A;
        case GLFW_KEY_B:             return Key::B;
        case GLFW_KEY_C:             return Key::C;
        case GLFW_KEY_D:             return Key::D;
        case GLFW_KEY_E:             return Key::E;
        case GLFW_KEY_F:             return Key::F;
        case GLFW_KEY_G:             return Key::G;
        case GLFW_KEY_H:             return Key::H;
        case GLFW_KEY_I:             return Key::I;
        case GLFW_KEY_J:             return Key::J;
        case GLFW_KEY_K:             return Key::K;
        case GLFW_KEY_L:             return Key::L;
        case GLFW_KEY_M:             return Key::M;
        case GLFW_KEY_N:             return Key::N;
        case GLFW_KEY_O:             return Key::O;
        case GLFW_KEY_P:             return Key::P;
        case GLFW_KEY_Q:             return Key::Q;
        case GLFW_KEY_R:             return Key::R;
        case GLFW_KEY_S:             return Key::S;
        case GLFW_KEY_T:             return Key::T;
        case GLFW_KEY_U:             return Key::U;
        case GLFW_KEY_V:             return Key::V;
        case GLFW_KEY_W:             return Key::W;
        case GLFW_KEY_X:             return Key::X;
        case GLFW_KEY_Y:             return Key::Y;
        case GLFW_KEY_Z:             return Key::Z;
        case GLFW_KEY_LEFT_BRACKET:  return Key::Left_Bracket;
        case GLFW_KEY_BACKSLASH:     return Key::Backslash;
        case GLFW_KEY_RIGHT_BRACKET: return Key::Right_Bracket;
        case GLFW_KEY_GRAVE_ACCENT:  return Key::Grave_Accent;
        case GLFW_KEY_ESCAPE:        return Key::Escape;
        case GLFW_KEY_ENTER:         return Key::Enter;
        case GLFW_KEY_TAB:           return Key::Tab;
        case GLFW_KEY_BACKSPACE:     return Key::Backspace;
        case GLFW_KEY_INSERT:        return Key::Insert;
        case GLFW_KEY_DELETE:        return Key::Delete;
        case GLFW_KEY_RIGHT:         return Key::Arrow_Right;
        case GLFW_KEY_LEFT:          return Key::Arrow_Left;
        case GLFW_KEY_DOWN:          return Key::Arrow_Down;
        case GLFW_KEY_UP:            return Key::Arrow_Up;
        case GLFW_KEY_PAGE_UP:       return Key::Page_Up;
        case GLFW_KEY_PAGE_DOWN:     return Key::Page_Down;
        case GLFW_KEY_HOME:          return Key::Home;
        case GLFW_KEY_END:           return Key::End;
        case GLFW_KEY_CAPS_LOCK:     return Key::Caps_Lock;
        case GLFW_KEY_SCROLL_LOCK:   return Key::Scroll_Lock;
        case GLFW_KEY_NUM_LOCK:      return Key::Num_Lock;
        case GLFW_KEY_PRINT_SCREEN:  return Key::Print_Screen;
        case GLFW_KEY_PAUSE:         return Key::Pause;
        case GLFW_KEY_F1:            return Key::F1;
        case GLFW_KEY_F2:            return Key::F2;
        case GLFW_KEY_F3:            return Key::F3;
        case GLFW_KEY_F4:            return Key::F4;
        case GLFW_KEY_F5:            return Key::F5;
        case GLFW_KEY_F6:            return Key::F6;
        case GLFW_KEY_F7:            return Key::F7;
        case GLFW_KEY_F8:            return Key::F8;
        case GLFW_KEY_F9:            return Key::F9;
        case GLFW_KEY_F10:           return Key::F10;
        case GLFW_KEY_F11:           return Key::F11;
        case GLFW_KEY_F12:           return Key::F12;
        case GLFW_KEY_LEFT_SHIFT:    return Key::Left_Shift;
        case GLFW_KEY_LEFT_CONTROL:  return Key::Left_Control;
        case GLFW_KEY_LEFT_ALT:      return Key::Left_Alt;
        case GLFW_KEY_LEFT_SUPER:    return Key::Left_Super;
        case GLFW_KEY_RIGHT_SHIFT:   return Key::Right_Shift;
        case GLFW_KEY_RIGHT_CONTROL: return Key::Right_Control;
        case GLFW_KEY_RIGHT_ALT:     return Key::Right_Alt;
        case GLFW_KEY_RIGHT_SUPER:   return Key::Right_Super;
        case GLFW_KEY_MENU:          return Key::Menu;
        case GLFW_KEY_KP_0:          return Key::Numpad_0;
        case GLFW_KEY_KP_1:          return Key::Numpad_1;
        case GLFW_KEY_KP_2:          return Key::Numpad_2;
        case GLFW_KEY_KP_3:          return Key::Numpad_3;
        case GLFW_KEY_KP_4:          return Key::Numpad_4;
        case GLFW_KEY_KP_5:          return Key::Numpad_5;
        case GLFW_KEY_KP_6:          return Key::Numpad_6;
        case GLFW_KEY_KP_7:          return Key::Numpad_7;
        case GLFW_KEY_KP_8:          return Key::Numpad_8;
        case GLFW_KEY_KP_9:          return Key::Numpad_9;
        case GLFW_KEY_KP_DECIMAL:    return Key::Numpad_Decimal;
        case GLFW_KEY_KP_DIVIDE:     return Key::Numpad_Divide;
        case GLFW_KEY_KP_MULTIPLY:   return Key::Numpad_Multiply;
        case GLFW_KEY_KP_SUBTRACT:   return Key::Numpad_Subtract;
        case GLFW_KEY_KP_ADD:        return Key::Numpad_Add;
        case GLFW_KEY_KP_ENTER:      return Key::Numpad_Enter;
        case GLFW_KEY_KP_EQUAL:      return Key::Numpad_Equal;
        default:                     return Key::Unknown;
        }
    }

    Mouse_Button Input::Glfw_button_to_btn(int _glfw_button)
    {
        switch (_glfw_button)
        {
        case GLFW_MOUSE_BUTTON_LEFT:   return Mouse_Button::Left;
        case GLFW_MOUSE_BUTTON_RIGHT:  return Mouse_Button::Right;
        case GLFW_MOUSE_BUTTON_MIDDLE: return Mouse_Button::Middle;
        case GLFW_MOUSE_BUTTON_4:      return Mouse_Button::Extra1;
        case GLFW_MOUSE_BUTTON_5:      return Mouse_Button::Extra2;
        default:                       return Mouse_Button::COUNT; // sentinel for unknown
        }
    }

    // =========================================================
    // String ↔ Key translation (for JSON action loading)
    // =========================================================

    Key Input::String_to_key(const std::string& _str)
    {
        static const std::unordered_map<std::string, Key> table =
        {
            { "Space",         Key::Space         },
            { "Apostrophe",    Key::Apostrophe    },
            { "Comma",         Key::Comma         },
            { "Minus",         Key::Minus         },
            { "Period",        Key::Period        },
            { "Slash",         Key::Slash         },
            { "Num_0",         Key::Num_0         },
            { "Num_1",         Key::Num_1         },
            { "Num_2",         Key::Num_2         },
            { "Num_3",         Key::Num_3         },
            { "Num_4",         Key::Num_4         },
            { "Num_5",         Key::Num_5         },
            { "Num_6",         Key::Num_6         },
            { "Num_7",         Key::Num_7         },
            { "Num_8",         Key::Num_8         },
            { "Num_9",         Key::Num_9         },
            { "Semicolon",     Key::Semicolon     },
            { "Equal",         Key::Equal         },
            { "A",             Key::A             },
            { "B",             Key::B             },
            { "C",             Key::C             },
            { "D",             Key::D             },
            { "E",             Key::E             },
            { "F",             Key::F             },
            { "G",             Key::G             },
            { "H",             Key::H             },
            { "I",             Key::I             },
            { "J",             Key::J             },
            { "K",             Key::K             },
            { "L",             Key::L             },
            { "M",             Key::M             },
            { "N",             Key::N             },
            { "O",             Key::O             },
            { "P",             Key::P             },
            { "Q",             Key::Q             },
            { "R",             Key::R             },
            { "S",             Key::S             },
            { "T",             Key::T             },
            { "U",             Key::U             },
            { "V",             Key::V             },
            { "W",             Key::W             },
            { "X",             Key::X             },
            { "Y",             Key::Y             },
            { "Z",             Key::Z             },
            { "Left_Bracket",  Key::Left_Bracket  },
            { "Backslash",     Key::Backslash     },
            { "Right_Bracket", Key::Right_Bracket },
            { "Grave_Accent",  Key::Grave_Accent  },
            { "Escape",        Key::Escape        },
            { "Enter",         Key::Enter         },
            { "Tab",           Key::Tab           },
            { "Backspace",     Key::Backspace     },
            { "Insert",        Key::Insert        },
            { "Delete",        Key::Delete        },
            { "Arrow_Right",   Key::Arrow_Right   },
            { "Arrow_Left",    Key::Arrow_Left    },
            { "Arrow_Down",    Key::Arrow_Down    },
            { "Arrow_Up",      Key::Arrow_Up      },
            { "Page_Up",       Key::Page_Up       },
            { "Page_Down",     Key::Page_Down     },
            { "Home",          Key::Home          },
            { "End",           Key::End           },
            { "Caps_Lock",     Key::Caps_Lock     },
            { "Scroll_Lock",   Key::Scroll_Lock   },
            { "Num_Lock",      Key::Num_Lock      },
            { "Print_Screen",  Key::Print_Screen  },
            { "Pause",         Key::Pause         },
            { "F1",            Key::F1            },
            { "F2",            Key::F2            },
            { "F3",            Key::F3            },
            { "F4",            Key::F4            },
            { "F5",            Key::F5            },
            { "F6",            Key::F6            },
            { "F7",            Key::F7            },
            { "F8",            Key::F8            },
            { "F9",            Key::F9            },
            { "F10",           Key::F10           },
            { "F11",           Key::F11           },
            { "F12",           Key::F12           },
            { "Left_Shift",    Key::Left_Shift    },
            { "Left_Control",  Key::Left_Control  },
            { "Left_Alt",      Key::Left_Alt      },
            { "Left_Super",    Key::Left_Super    },
            { "Right_Shift",   Key::Right_Shift   },
            { "Right_Control", Key::Right_Control },
            { "Right_Alt",     Key::Right_Alt     },
            { "Right_Super",   Key::Right_Super   },
            { "Menu",          Key::Menu          },
            { "Numpad_0",      Key::Numpad_0      },
            { "Numpad_1",      Key::Numpad_1      },
            { "Numpad_2",      Key::Numpad_2      },
            { "Numpad_3",      Key::Numpad_3      },
            { "Numpad_4",      Key::Numpad_4      },
            { "Numpad_5",      Key::Numpad_5      },
            { "Numpad_6",      Key::Numpad_6      },
            { "Numpad_7",      Key::Numpad_7      },
            { "Numpad_8",      Key::Numpad_8      },
            { "Numpad_9",      Key::Numpad_9      },
            { "Numpad_Decimal",  Key::Numpad_Decimal  },
            { "Numpad_Divide",   Key::Numpad_Divide   },
            { "Numpad_Multiply", Key::Numpad_Multiply },
            { "Numpad_Subtract", Key::Numpad_Subtract },
            { "Numpad_Add",      Key::Numpad_Add      },
            { "Numpad_Enter",    Key::Numpad_Enter    },
            { "Numpad_Equal",    Key::Numpad_Equal    },
        };

        auto it = table.find(_str);
        if (it == table.end())
        {
            std::cout << "[Input] Warning: unknown key string '" << _str << "'\n";
            return Key::Unknown;
        }
        return it->second;
    }

    Mouse_Button Input::String_to_mouse_button(const std::string& _str)
    {
        if (_str == "Left_Mouse" || _str == "Mouse_Left")   return Mouse_Button::Left;
        if (_str == "Right_Mouse" || _str == "Mouse_Right")  return Mouse_Button::Right;
        if (_str == "Middle_Mouse" || _str == "Mouse_Middle") return Mouse_Button::Middle;
        if (_str == "Mouse_Extra1")                           return Mouse_Button::Extra1;
        if (_str == "Mouse_Extra2")                           return Mouse_Button::Extra2;

        std::cout << "[Input] Warning: unknown mouse button string '" << _str << "'\n";
        return Mouse_Button::Left;
    }

} // namespace Input