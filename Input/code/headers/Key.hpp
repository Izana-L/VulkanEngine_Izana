#pragma once

#include <cstdint>

namespace Input
{

    // Key: keyboard key identifiers, independent of GLFW.
    // If the windowing backend changes, only the translation layer
    // in Input.cpp (Glfw_key_to_key / Key_to_glfw_key) needs updating —
    // nothing in the rest of the engine.
    enum class Key : uint16_t
    {
        // ── Unknown ───────────────────────────────────────────────
        Unknown = 0,

        // ── Printable keys ────────────────────────────────────────
        Space,
        Apostrophe,
        Comma,
        Minus,
        Period,
        Slash,

        Num_0, Num_1, Num_2, Num_3, Num_4,
        Num_5, Num_6, Num_7, Num_8, Num_9,

        Semicolon,
        Equal,

        A, B, C, D, E, F, G, H, I, J, K, L, M,
        N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

        Left_Bracket,
        Backslash,
        Right_Bracket,
        Grave_Accent,

        // ── Function keys ─────────────────────────────────────────
        F1, F2, F3, F4, F5, F6,
        F7, F8, F9, F10, F11, F12,

        // ── Navigation ────────────────────────────────────────────
        Escape,
        Enter,
        Tab,
        Backspace,
        Insert,
        Delete,
        Arrow_Right,
        Arrow_Left,
        Arrow_Down,
        Arrow_Up,
        Page_Up,
        Page_Down,
        Home,
        End,

        // ── Modifiers ─────────────────────────────────────────────
        Caps_Lock,
        Scroll_Lock,
        Num_Lock,
        Print_Screen,
        Pause,

        Left_Shift,
        Left_Control,
        Left_Alt,
        Left_Super,
        Right_Shift,
        Right_Control,
        Right_Alt,
        Right_Super,
        Menu,

        // ── Numpad ────────────────────────────────────────────────
        Numpad_0, Numpad_1, Numpad_2, Numpad_3, Numpad_4,
        Numpad_5, Numpad_6, Numpad_7, Numpad_8, Numpad_9,
        Numpad_Decimal,
        Numpad_Divide,
        Numpad_Multiply,
        Numpad_Subtract,
        Numpad_Add,
        Numpad_Enter,
        Numpad_Equal,

        COUNT   // total number of keys — used to size arrays
    };

    // Mouse_Button: mouse button identifiers.
    enum class Mouse_Button : uint8_t
    {
        Left = 0,
        Right = 1,
        Middle = 2,
        Extra1 = 3,
        Extra2 = 4,

        COUNT
    };

    // Cursor_Mode: controls cursor visibility and capture.
    enum class Cursor_Mode : uint8_t
    {
        // Cursor visible and free — for UI interaction.
        Window,

        // Cursor hidden and captured — for camera rotation.
        // Mouse delta is meaningful; absolute position is not.
        Camera
    };

} // namespace Input