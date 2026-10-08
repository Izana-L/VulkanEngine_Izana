#pragma once

#include <string>
#include <cstdint>
#include <functional>

// Opaque GLFW window type: only pointers to it appear in this header, so
// including Window.hpp does not pull in GLFW (or Vulkan).
struct GLFWwindow;

namespace Platform {

    // Window: wraps a GLFW window and exposes the minimal interface
    // the rest of the engine needs to create a Vulkan surface, run the
    // main loop, and manage window state (fullscreen, focus, position...).
    class Window
    {
    public:
        // Value of a size limit that is not set (equal to GLFW_DONT_CARE,
        // which Window.cpp checks at compile time).
        static constexpr int no_size_limit = -1;

    private:
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

        // Size limits currently applied to the GLFW window. GLFW only has one
        // call that sets all four at once, so every change has to start from
        // these values or it would reset the ones it does not mention.
        int min_width  = no_size_limit;
        int min_height = no_size_limit;
        int max_width  = no_size_limit;
        int max_height = no_size_limit;

        // Destroys the GLFW window, if any, and releases its reference to the
        // GLFW library (which terminates GLFW when it was the last one).
        // Shared by the destructor and move assignment. Leaves window_handle
        // null.
        void Destroy_native();

        // Validates the four limits as a set and applies them with a single
        // glfwSetWindowSizeLimits call. Stores them only if GLFW received
        // them, so the members always match what is in effect. Returns false,
        // changing nothing, if the set is invalid.
        bool Apply_size_limits(int _min_width, int _min_height, int _max_width, int _max_height);

        // Shared body of the move constructor and move assignment: takes
        // ownership of every field of _other, the input callbacks included,
        // and re-points the GLFW user pointer at this object.
        void Move_from(Window&& _other) noexcept;

        // Saves the current position and size so they can be restored when
        // leaving fullscreen. Only records anything while the window is
        // actually windowed: saving while fullscreen or borderless would
        // overwrite the real geometry with the monitor's.
        void Remember_windowed_geometry();

        // Applies the saved geometry and restores decorations.
        void Restore_windowed_geometry();

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
        // where polling in a loop would just burn a core. Not const: it
        // dispatches the callbacks, which modify the window (resize flag,
        // input state), so only the owner of the main loop calls it.
        void Wait_events();
        // Like Wait_events(), but returns after at most _seconds even if no
        // event arrived (a finite, non-negative value; anything else counts
        // as 0, i.e. a poll). For idle states where the loop must keep
        // running its systems at a modest rate, such as a scene without a
        // camera: it sleeps instead of spinning, and still wakes up at once
        // when an event arrives.
        void Wait_events_timeout(double _seconds);
        // =========================================================
        // Size
        // =========================================================

        void Get_size(int& _out_width, int& _out_height) const;
        void Get_framebuffer_size(int& _out_width, int& _out_height) const;

        // True once after every framebuffer resize reported by GLFW, then
        // false until the next one. The engine loop forwards it to the
        // Renderer so the swapchain is rebuilt on the next frame, without
        // depending on the driver returning OUT_OF_DATE / SUBOPTIMAL.
        bool Consume_resized_flag();

        // Smallest / largest size the user can resize the window to. The two
        // limits are independent: setting one never changes the other. Values
        // are in screen coordinates and must be >= 0. They apply to windowed,
        // resizable windows.
        //
        // Both return false, leaving every limit as it was, if the request is
        // rejected: a negative value, or a minimum larger than the maximum
        // (in either dimension) that is currently set. To move the range past
        // the other limit, change that one first or call Clear_size_limits().
        bool Set_min_size(int _min_width, int _min_height);
        bool Set_max_size(int _max_width, int _max_height);

        // Removes both the minimum and the maximum size limit.
        void Clear_size_limits();

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