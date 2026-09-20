#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>
#include <cstdint>
#include <functional>

namespace Platform {

    // Window: wraps a GLFW window and exposes the minimal interface
    // the rest of the engine needs to create a Vulkan surface, run the
    // main loop, and manage window state (fullscreen, focus, position...).
    class Window
    {
        GLFWwindow* window_handle;
        uint32_t    width;
        uint32_t    height;
        std::string title;
        bool        was_resized;

        int  windowed_pos_x;
        int  windowed_pos_y;
        int  windowed_width;
        int  windowed_height;
        bool is_fullscreen;
        bool is_windowed_fullscreen;

        static int window_count;

    public:

        Window(uint32_t _width, uint32_t _height, const std::string& _title);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&& _other) noexcept;
        Window& operator=(Window&& _other) noexcept;

        // =========================================================
        // Core loop
        // =========================================================

        bool Should_close() const;
        void Poll_events();
        // Blocks until an OS event arrives. For idle states (minimized)
        // where polling in a loop would just burn a core.
        void Wait_events();
        // =========================================================
        // Size
        // =========================================================

        void Get_size(int& _out_width, int& _out_height) const;
        void Get_framebuffer_size(int& _out_width, int& _out_height) const;
        bool Consume_resized_flag();
        void Set_min_size(int _min_width, int _min_height);
        void Set_max_size(int _max_width, int _max_height);
        void Set_resizable(bool _resizable);

        // =========================================================
        // Title and icon
        // =========================================================

        void               Set_title(const std::string& _title);
        const std::string& Get_title() const;
        void               Set_icon(const unsigned char* _pixels, int _width, int _height);

        // =========================================================
        // Position
        // =========================================================

        void Set_position(int _x, int _y);
        void Get_position(int& _out_x, int& _out_y) const;
        void Center();

        // =========================================================
        // Window state
        // =========================================================

        void Minimize();
        void Maximize();
        void Restore();
        void Set_decorated(bool _decorated);
        bool Is_decorated() const;
        bool Is_minimized() const;
        bool Is_maximized() const;
        bool Is_focused() const;
        bool Is_visible() const;
        void Focus();
        void Show();
        void Hide();

        // =========================================================
        // Fullscreen
        // =========================================================

        void Set_fullscreen(bool _fullscreen);
        bool Is_fullscreen() const;
        void Set_windowed_fullscreen(bool _enabled);
        bool Is_windowed_fullscreen() const;

        // =========================================================
        // Monitor / DPI
        // =========================================================

        void        Get_content_scale(float& _out_scale_x, float& _out_scale_y) const;
        static int  Get_monitor_count();

        // =========================================================
        // Native access
        // =========================================================

        GLFWwindow* Get_native_handle() const;

        // =========================================================
        // Callbacks — window events
        // =========================================================

        using Close_callback = std::function<void()>;
        using Focus_callback = std::function<void(bool _focused)>;
        using Iconify_callback = std::function<void(bool _minimized)>;
        using Position_callback = std::function<void(int _x, int _y)>;

        void Set_close_callback(Close_callback    _callback);
        void Set_focus_callback(Focus_callback    _callback);
        void Set_iconify_callback(Iconify_callback  _callback);
        void Set_position_callback(Position_callback _callback);

        // =========================================================
        // Callbacks — input events (for Input to subscribe to)
        // =========================================================

        // key:    GLFW key code, action: GLFW_PRESS / GLFW_RELEASE / GLFW_REPEAT
        using Key_callback = std::function<void(int _key, int _action)>;

        // button: GLFW mouse button, action: GLFW_PRESS / GLFW_RELEASE
        using Mouse_button_callback = std::function<void(int _button, int _action)>;

        // xpos, ypos: cursor position in screen coordinates
        using Mouse_move_callback = std::function<void(double _xpos, double _ypos)>;

        // yoffset: scroll wheel delta (positive = up)
        using Scroll_callback = std::function<void(double _yoffset)>;

        void Set_key_callback(Key_callback          _callback);
        void Set_mouse_button_callback(Mouse_button_callback _callback);
        void Set_mouse_move_callback(Mouse_move_callback   _callback);
        void Set_scroll_callback(Scroll_callback       _callback);

    private:

        // ── Window GLFW callbacks ──────────────────────────────────
        static void Framebuffer_resize_callback(GLFWwindow* _w, int _width, int _height);
        static void Window_close_callback(GLFWwindow* _w);
        static void Window_focus_callback(GLFWwindow* _w, int _focused);
        static void Window_iconify_callback(GLFWwindow* _w, int _iconified);
        static void Window_position_callback(GLFWwindow* _w, int _x, int _y);

        // ── Input GLFW callbacks ───────────────────────────────────
        static void Key_callback_internal(GLFWwindow* _w, int _key, int _scancode, int _action, int _mods);
        static void Mouse_button_callback_internal(GLFWwindow* _w, int _button, int _action, int _mods);
        static void Mouse_move_callback_internal(GLFWwindow* _w, double _xpos, double _ypos);
        static void Scroll_callback_internal(GLFWwindow* _w, double _xoffset, double _yoffset);

        // ── Window callback members ────────────────────────────────
        Close_callback    close_callback;
        Focus_callback    focus_callback;
        Iconify_callback  iconify_callback;
        Position_callback position_callback;

        // ── Input callback members ─────────────────────────────────
        Key_callback          key_callback;
        Mouse_button_callback mouse_button_callback;
        Mouse_move_callback   mouse_move_callback;
        Scroll_callback       scroll_callback;
    };

} // namespace Platform