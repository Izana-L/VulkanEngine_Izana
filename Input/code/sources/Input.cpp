#include <Input.hpp>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <json.hpp>

#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace Input_System
{

    // =========================================================
    // Key / mouse button tables - SINGLE SOURCE OF TRUTH
    // =========================================================
    //
    // One row per Key: its JSON name and its GLFW code. Every translation
    // (GLFW -> Key, name -> Key, Key -> name) is derived from this table,
    // so a key can no longer exist in one direction and not the other.
    // The static_assert below fails the build if a Key is added to the
    // enum without a row here.

    namespace
    {
        struct Key_Entry
        {
            Key         key;
            const char* name;
            int         glfw_key;
        };

        constexpr Key_Entry key_table[] =
        {
            { Key::Space,           "Space",           GLFW_KEY_SPACE         },
            { Key::Apostrophe,      "Apostrophe",      GLFW_KEY_APOSTROPHE    },
            { Key::Comma,           "Comma",           GLFW_KEY_COMMA         },
            { Key::Minus,           "Minus",           GLFW_KEY_MINUS         },
            { Key::Period,          "Period",          GLFW_KEY_PERIOD        },
            { Key::Slash,           "Slash",           GLFW_KEY_SLASH         },
            { Key::Num_0,           "Num_0",           GLFW_KEY_0             },
            { Key::Num_1,           "Num_1",           GLFW_KEY_1             },
            { Key::Num_2,           "Num_2",           GLFW_KEY_2             },
            { Key::Num_3,           "Num_3",           GLFW_KEY_3             },
            { Key::Num_4,           "Num_4",           GLFW_KEY_4             },
            { Key::Num_5,           "Num_5",           GLFW_KEY_5             },
            { Key::Num_6,           "Num_6",           GLFW_KEY_6             },
            { Key::Num_7,           "Num_7",           GLFW_KEY_7             },
            { Key::Num_8,           "Num_8",           GLFW_KEY_8             },
            { Key::Num_9,           "Num_9",           GLFW_KEY_9             },
            { Key::Semicolon,       "Semicolon",       GLFW_KEY_SEMICOLON     },
            { Key::Equal,           "Equal",           GLFW_KEY_EQUAL         },
            { Key::A,               "A",               GLFW_KEY_A             },
            { Key::B,               "B",               GLFW_KEY_B             },
            { Key::C,               "C",               GLFW_KEY_C             },
            { Key::D,               "D",               GLFW_KEY_D             },
            { Key::E,               "E",               GLFW_KEY_E             },
            { Key::F,               "F",               GLFW_KEY_F             },
            { Key::G,               "G",               GLFW_KEY_G             },
            { Key::H,               "H",               GLFW_KEY_H             },
            { Key::I,               "I",               GLFW_KEY_I             },
            { Key::J,               "J",               GLFW_KEY_J             },
            { Key::K,               "K",               GLFW_KEY_K             },
            { Key::L,               "L",               GLFW_KEY_L             },
            { Key::M,               "M",               GLFW_KEY_M             },
            { Key::N,               "N",               GLFW_KEY_N             },
            { Key::O,               "O",               GLFW_KEY_O             },
            { Key::P,               "P",               GLFW_KEY_P             },
            { Key::Q,               "Q",               GLFW_KEY_Q             },
            { Key::R,               "R",               GLFW_KEY_R             },
            { Key::S,               "S",               GLFW_KEY_S             },
            { Key::T,               "T",               GLFW_KEY_T             },
            { Key::U,               "U",               GLFW_KEY_U             },
            { Key::V,               "V",               GLFW_KEY_V             },
            { Key::W,               "W",               GLFW_KEY_W             },
            { Key::X,               "X",               GLFW_KEY_X             },
            { Key::Y,               "Y",               GLFW_KEY_Y             },
            { Key::Z,               "Z",               GLFW_KEY_Z             },
            { Key::Left_Bracket,    "Left_Bracket",    GLFW_KEY_LEFT_BRACKET  },
            { Key::Backslash,       "Backslash",       GLFW_KEY_BACKSLASH     },
            { Key::Right_Bracket,   "Right_Bracket",   GLFW_KEY_RIGHT_BRACKET },
            { Key::Grave_Accent,    "Grave_Accent",    GLFW_KEY_GRAVE_ACCENT  },
            { Key::F1,              "F1",              GLFW_KEY_F1            },
            { Key::F2,              "F2",              GLFW_KEY_F2            },
            { Key::F3,              "F3",              GLFW_KEY_F3            },
            { Key::F4,              "F4",              GLFW_KEY_F4            },
            { Key::F5,              "F5",              GLFW_KEY_F5            },
            { Key::F6,              "F6",              GLFW_KEY_F6            },
            { Key::F7,              "F7",              GLFW_KEY_F7            },
            { Key::F8,              "F8",              GLFW_KEY_F8            },
            { Key::F9,              "F9",              GLFW_KEY_F9            },
            { Key::F10,             "F10",             GLFW_KEY_F10           },
            { Key::F11,             "F11",             GLFW_KEY_F11           },
            { Key::F12,             "F12",             GLFW_KEY_F12           },
            { Key::Escape,          "Escape",          GLFW_KEY_ESCAPE        },
            { Key::Enter,           "Enter",           GLFW_KEY_ENTER         },
            { Key::Tab,             "Tab",             GLFW_KEY_TAB           },
            { Key::Backspace,       "Backspace",       GLFW_KEY_BACKSPACE     },
            { Key::Insert,          "Insert",          GLFW_KEY_INSERT        },
            { Key::Delete,          "Delete",          GLFW_KEY_DELETE        },
            { Key::Arrow_Right,     "Arrow_Right",     GLFW_KEY_RIGHT         },
            { Key::Arrow_Left,      "Arrow_Left",      GLFW_KEY_LEFT          },
            { Key::Arrow_Down,      "Arrow_Down",      GLFW_KEY_DOWN          },
            { Key::Arrow_Up,        "Arrow_Up",        GLFW_KEY_UP            },
            { Key::Page_Up,         "Page_Up",         GLFW_KEY_PAGE_UP       },
            { Key::Page_Down,       "Page_Down",       GLFW_KEY_PAGE_DOWN     },
            { Key::Home,            "Home",            GLFW_KEY_HOME          },
            { Key::End,             "End",             GLFW_KEY_END           },
            { Key::Caps_Lock,       "Caps_Lock",       GLFW_KEY_CAPS_LOCK     },
            { Key::Scroll_Lock,     "Scroll_Lock",     GLFW_KEY_SCROLL_LOCK   },
            { Key::Num_Lock,        "Num_Lock",        GLFW_KEY_NUM_LOCK      },
            { Key::Print_Screen,    "Print_Screen",    GLFW_KEY_PRINT_SCREEN  },
            { Key::Pause,           "Pause",           GLFW_KEY_PAUSE         },
            { Key::Left_Shift,      "Left_Shift",      GLFW_KEY_LEFT_SHIFT    },
            { Key::Left_Control,    "Left_Control",    GLFW_KEY_LEFT_CONTROL  },
            { Key::Left_Alt,        "Left_Alt",        GLFW_KEY_LEFT_ALT      },
            { Key::Left_Super,      "Left_Super",      GLFW_KEY_LEFT_SUPER    },
            { Key::Right_Shift,     "Right_Shift",     GLFW_KEY_RIGHT_SHIFT   },
            { Key::Right_Control,   "Right_Control",   GLFW_KEY_RIGHT_CONTROL },
            { Key::Right_Alt,       "Right_Alt",       GLFW_KEY_RIGHT_ALT     },
            { Key::Right_Super,     "Right_Super",     GLFW_KEY_RIGHT_SUPER   },
            { Key::Menu,            "Menu",            GLFW_KEY_MENU          },
            { Key::Numpad_0,        "Numpad_0",        GLFW_KEY_KP_0          },
            { Key::Numpad_1,        "Numpad_1",        GLFW_KEY_KP_1          },
            { Key::Numpad_2,        "Numpad_2",        GLFW_KEY_KP_2          },
            { Key::Numpad_3,        "Numpad_3",        GLFW_KEY_KP_3          },
            { Key::Numpad_4,        "Numpad_4",        GLFW_KEY_KP_4          },
            { Key::Numpad_5,        "Numpad_5",        GLFW_KEY_KP_5          },
            { Key::Numpad_6,        "Numpad_6",        GLFW_KEY_KP_6          },
            { Key::Numpad_7,        "Numpad_7",        GLFW_KEY_KP_7          },
            { Key::Numpad_8,        "Numpad_8",        GLFW_KEY_KP_8          },
            { Key::Numpad_9,        "Numpad_9",        GLFW_KEY_KP_9          },
            { Key::Numpad_Decimal,  "Numpad_Decimal",  GLFW_KEY_KP_DECIMAL    },
            { Key::Numpad_Divide,   "Numpad_Divide",   GLFW_KEY_KP_DIVIDE     },
            { Key::Numpad_Multiply, "Numpad_Multiply", GLFW_KEY_KP_MULTIPLY   },
            { Key::Numpad_Subtract, "Numpad_Subtract", GLFW_KEY_KP_SUBTRACT   },
            { Key::Numpad_Add,      "Numpad_Add",      GLFW_KEY_KP_ADD        },
            { Key::Numpad_Enter,    "Numpad_Enter",    GLFW_KEY_KP_ENTER      },
            { Key::Numpad_Equal,    "Numpad_Equal",    GLFW_KEY_KP_EQUAL      },
        };

        constexpr size_t key_table_size = sizeof(key_table) / sizeof(key_table[0]);

        // Every Key except Unknown must have exactly one row.
        static_assert(key_table_size == static_cast<size_t>(Key::COUNT) - 1,
            "key_table must list every Key except Key::Unknown");

        constexpr bool Key_table_is_ordered()
        {
            for (size_t i = 0; i < key_table_size; ++i)
                if (static_cast<size_t>(key_table[i].key) != i + 1) return false;
            return true;
        }

        // Rows are in enum order so Key_to_string is a direct index.
        static_assert(Key_table_is_ordered(), "key_table rows must follow the Key enum order");

        struct Mouse_Entry
        {
            Mouse_Button button;
            const char*  name;
            const char*  alias;    // second spelling accepted in JSON, or nullptr
            int          glfw_button;
        };

        constexpr Mouse_Entry mouse_table[] =
        {
            { Mouse_Button::Left,   "Mouse_Left",   "Left_Mouse",   GLFW_MOUSE_BUTTON_LEFT   },
            { Mouse_Button::Right,  "Mouse_Right",  "Right_Mouse",  GLFW_MOUSE_BUTTON_RIGHT  },
            { Mouse_Button::Middle, "Mouse_Middle", "Middle_Mouse", GLFW_MOUSE_BUTTON_MIDDLE },
            { Mouse_Button::Extra1, "Mouse_Extra1", nullptr,        GLFW_MOUSE_BUTTON_4      },
            { Mouse_Button::Extra2, "Mouse_Extra2", nullptr,        GLFW_MOUSE_BUTTON_5      },
        };

        constexpr size_t mouse_table_size = sizeof(mouse_table) / sizeof(mouse_table[0]);

        static_assert(mouse_table_size == static_cast<size_t>(Mouse_Button::COUNT),
            "mouse_table must list every Mouse_Button");

        // GLFW key codes are sparse (up to GLFW_KEY_LAST = 348), so the
        // GLFW -> Key direction is a flat array built once from the table.
        const std::array<Key, GLFW_KEY_LAST + 1>& Glfw_key_lookup()
        {
            static const std::array<Key, GLFW_KEY_LAST + 1> lookup = []
                {
                    std::array<Key, GLFW_KEY_LAST + 1> table{};
                    table.fill(Key::Unknown);
                    for (const Key_Entry& entry : key_table)
                        table[static_cast<size_t>(entry.glfw_key)] = entry.key;
                    return table;
                }();

            return lookup;
        }

        const std::unordered_map<std::string_view, Key>& Key_name_lookup()
        {
            static const std::unordered_map<std::string_view, Key> lookup = []
                {
                    std::unordered_map<std::string_view, Key> table;
                    table.reserve(key_table_size);
                    for (const Key_Entry& entry : key_table)
                        table.emplace(entry.name, entry.key);
                    return table;
                }();

            return lookup;
        }
    }

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

    Input::~Input()
    {
        window.Set_key_callback(nullptr);
        window.Set_mouse_button_callback(nullptr);
        window.Set_mouse_move_callback(nullptr);
        window.Set_scroll_callback(nullptr);
    }

    void Input::Begin_frame()
    {
        previous_keys = current_keys;
        previous_buttons = current_buttons;
    }

    // =========================================================
    // Update - call once per frame after Poll_events()
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

    void Input::Discard_pending()
    {
        Clear_motion();

        // The next motion event re-anchors the absolute position instead
        // of producing a delta against a position from before the pause.
        first_mouse = true;

        // Re-sync the edge snapshots with the present state, then compute
        // the action values from it and drop the edges: a key pressed and
        // released while nothing was listening never happened.
        previous_keys = current_keys;
        previous_buttons = current_buttons;

        Update_actions();

        for (Action& action : actions)
        {
            action.pressed = false;
            action.released = false;
        }
    }

    void Input::Clear_motion()
    {
        mouse_delta_x = 0.0f;
        mouse_delta_y = 0.0f;
        mouse_delta_x_acc = 0.0f;
        mouse_delta_y_acc = 0.0f;
        scroll_delta = 0.0f;
        scroll_accumulator = 0.0f;
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

        // Both the delta already published for this frame and anything
        // accumulated since are dropped: the motion that accompanied the
        // toggle belongs to the previous mode. first_mouse makes the next
        // event re-anchor the position, because GLFW switches between
        // real and virtual cursor coordinates when the mode changes.
        Clear_motion();
        first_mouse = true;

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

        try
        {
            file >> json;
        }
        catch (const nlohmann::json::exception& e)
        {
            throw std::runtime_error("Input: action file '" + _path + "' is not valid JSON: " + e.what());
        }

        // Parse into a local list first, so a bad file leaves the
        // previously loaded actions untouched.
        std::vector<Action>                     parsed;
        std::unordered_map<std::string, size_t> parsed_index;

        try
        {
            for (const auto& entry : json.at("actions"))
            {
                Action action;
                action.name = entry.at("name").get<std::string>();

                if (parsed_index.count(action.name) != 0)
                    throw std::runtime_error("Input: action '" + action.name + "' is defined twice in '" + _path + "'");

                for (const auto& binding_json : entry.at("bindings"))
                {
                    const std::string str = binding_json.get<std::string>();

                    // Mouse names are looked up first, then keys. Anything
                    // else is a typo in the file, and a typo must not bind
                    // the action to an arbitrary key or button.
                    const Mouse_Button button = String_to_mouse_button(str);

                    if (button != Mouse_Button::COUNT)
                    {
                        action.bindings.push_back(Action_Binding::From_mouse_button(button));
                        continue;
                    }

                    const Key key = String_to_key(str);

                    if (key == Key::Unknown)
                    {
                        throw std::runtime_error(
                            "Input: action '" + action.name + "' in '" + _path +
                            "' has an unknown binding '" + str + "'");
                    }

                    action.bindings.push_back(Action_Binding::From_key(key));
                }

                parsed_index[action.name] = parsed.size();
                parsed.push_back(std::move(action));
            }
        }
        catch (const nlohmann::json::exception& e)
        {
            throw std::runtime_error("Input: action file '" + _path + "' has an unexpected layout: " + e.what());
        }

        actions = std::move(parsed);
        action_index = std::move(parsed_index);

        std::cout << "[Input] Loaded " << actions.size()
            << " action(s) from '" << _path << "'\n";
    }

    float Input::Get_action_value(const std::string& _action) const
    {
        auto it = action_index.find(_action);
        if (it == action_index.end()) return 0.0f;
        return actions[it->second].value;
    }

    size_t Input::Get_action_id(const std::string& _action) const
    {
        auto it = action_index.find(_action);
        return (it == action_index.end()) ? INVALID_ACTION : it->second;
    }

    float Input::Get_action_value(size_t _action_id) const
    {
        // INVALID_ACTION is ~0, so it fails this test and returns 0 -
        // same behaviour as the string version with an unknown name.
        return (_action_id < actions.size()) ? actions[_action_id].value : 0.0f;
    }

    bool Input::Is_action_down(size_t _action_id) const
    {
        return Get_action_value(_action_id) > 0.5f;
    }

    bool Input::Was_action_pressed(size_t _action_id) const
    {
        return (_action_id < actions.size()) ? actions[_action_id].pressed : false;
    }

    bool Input::Was_action_released(size_t _action_id) const
    {
        return (_action_id < actions.size()) ? actions[_action_id].released : false;
    }

    bool Input::Is_action_down(const std::string& _action) const
    {
        return Get_action_value(_action) > 0.5f;
    }

    bool Input::Was_action_pressed(const std::string& _action) const
    {
        return Was_action_pressed(Get_action_id(_action));
    }

    bool Input::Was_action_released(const std::string& _action) const
    {
        return Was_action_released(Get_action_id(_action));
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
        if (btn == Mouse_Button::COUNT) return;

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
    // Translation (all derived from the tables above)
    // =========================================================

    Key Input::Glfw_key_to_key(int _glfw_key)
    {
        if (_glfw_key < 0 || _glfw_key > GLFW_KEY_LAST) return Key::Unknown;

        return Glfw_key_lookup()[static_cast<size_t>(_glfw_key)];
    }

    Mouse_Button Input::Glfw_button_to_btn(int _glfw_button)
    {
        for (const Mouse_Entry& entry : mouse_table)
            if (entry.glfw_button == _glfw_button) return entry.button;

        return Mouse_Button::COUNT; // sentinel for unknown
    }

    const char* Input::Key_to_string(Key _key)
    {
        const size_t index = static_cast<size_t>(_key);

        if (index == 0 || index > key_table_size) return "Unknown";

        return key_table[index - 1].name;
    }

    const char* Input::Mouse_button_to_string(Mouse_Button _button)
    {
        const size_t index = static_cast<size_t>(_button);

        if (index >= mouse_table_size) return "Unknown";

        return mouse_table[index].name;
    }

    Key Input::String_to_key(const std::string& _str)
    {
        const auto& lookup = Key_name_lookup();

        auto it = lookup.find(std::string_view(_str));
        return (it == lookup.end()) ? Key::Unknown : it->second;
    }

    Mouse_Button Input::String_to_mouse_button(const std::string& _str)
    {
        for (const Mouse_Entry& entry : mouse_table)
        {
            if (_str == entry.name) return entry.button;
            if (entry.alias && _str == entry.alias) return entry.button;
        }

        return Mouse_Button::COUNT;
    }

} // namespace Input_System
