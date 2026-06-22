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
        uint32_t width;
        uint32_t height;
        std::string title;
        bool was_resized;

        // Stores window position/size before switching to fullscreen,
        // so it can be restored when switching back to windowed mode.
        int windowed_pos_x;
        int windowed_pos_y;
        int windowed_width;
        int windowed_height;
        bool is_fullscreen;
        bool is_windowed_fullscreen;


        // Tracks how many Window instances exist, so GLFW is initialized
        // only once and terminated only when the last window is destroyed.
        static int window_count;

    public:
        // Creates and opens a new window with the given dimensions and title.
        // Initializes GLFW internally if this is the first Window created.
        Window(uint32_t _width, uint32_t _height, const std::string& _title);

        // Destroys the window and terminates GLFW if this was the last
        // active Window instance.
        ~Window();

        // Windows should not be copied (GLFWwindow* ownership would be
        // duplicated, leading to double-destruction). Only allow moving.
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        Window(Window&& _other) noexcept;
        Window& operator=(Window&& _other) noexcept;

        // =========================================================
        // Core loop
        // =========================================================

        // Returns true if the user requested the window to close
        bool Should_close() const;

        // Processes pending OS events. Must be called once per frame.
        void Poll_events();

        // =========================================================
        // Size
        // =========================================================

        // Window size in screen coordinates (logical size, not pixels)
        void Get_size(int& _out_width, int& _out_height) const;

        // Actual framebuffer size in pixels - use this for Vulkan swapchain
        void Get_framebuffer_size(int& _out_width, int& _out_height) const;

        // Returns true if the window was resized since last checked, then resets the flag
        bool Consume_resized_flag();

        // Limits how much the user can resize the window (0 = no limit)
        void Set_min_size(int _min_width, int _min_height);
        void Set_max_size(int _max_width, int _max_height);

        // Allows/disallows the user from resizing the window at runtime
        void Set_resizable(bool _resizable);

        // =========================================================
        // Title and icon
        // =========================================================

        // Changes the window title at runtime (e.g. to show FPS or current scene)
        void Set_title(const std::string& _title);
        const std::string& Get_title() const;

        // Sets the window icon from a 32-bit RGBA pixel buffer.
        // pixels must be width*height*4 bytes (RGBA8, top-to-bottom rows).
        void Set_icon(const unsigned char* _pixels, int _width, int _height);

        // =========================================================
        // Position
        // =========================================================

        void Set_position(int _x, int _y);
        void Get_position(int& _out_x, int& _out_y) const;

        // Centers the window on its current monitor
        void Center();

        // =========================================================
        // Window state
        // =========================================================

        void Minimize();
        void Maximize();
        void Restore();

        // Shows or hides the native OS window decoration (title bar, borders,
        // and minimize/maximize/close buttons). When disabled, the window
        // becomes a plain rectangle with no title bar - common for custom
        // UI/editor windows that draw their own title bar.
        void Set_decorated(bool _decorated);
        bool Is_decorated() const;


        bool Is_minimized() const;
        bool Is_maximized() const;
        bool Is_focused() const;
        bool Is_visible() const;

        // Brings the window to the front and gives it input focus
        void Focus();

        void Show();
        void Hide();

        // =========================================================
        // Fullscreen
        // =========================================================

        // Switches to true exclusive fullscreen on the primary monitor
        void Set_fullscreen(bool _fullscreen);
        bool Is_fullscreen() const;

        // Switches to borderless fullscreen (a maximized, frameless window
        // covering the whole screen) - more alt-tab friendly than exclusive
        // fullscreen, commonly preferred in PC games.
        void Set_windowed_fullscreen(bool _enabled);
        bool Is_windowed_fullscreen() const;

        // =========================================================
        // Monitor / DPI
        // =========================================================

        // Returns the content scale factor for DPI awareness (1.0 = 100%)
        void Get_content_scale(float& _out_scale_x, float& _out_scale_y) const;

        // Returns how many monitors are connected to the system
        static int Get_monitor_count();

        // =========================================================
        // Native access
        // =========================================================

        // Raw GLFW handle, needed by the Renderer to create the Vulkan surface
        GLFWwindow* Get_native_handle() const;

        // =========================================================
        // Callbacks
        // =========================================================

        using Close_callback = std::function<void()>;
        using Focus_callback = std::function<void(bool _focused)>;
        using Iconify_callback = std::function<void(bool _minimized)>;
        using Position_callback = std::function<void(int _x, int _y)>;

        // Called when the user requests to close the window (X button, Alt+F4...).
        // Useful to intercept closing and show a confirmation dialog instead
        // of closing immediately.
        void Set_close_callback(Close_callback _callback);

        // Called when the window gains or loses input focus.
        // Useful to auto-pause the game when the user alt-tabs away.
        void Set_focus_callback(Focus_callback _callback);

        // Called when the window is minimized or restored.
        // Useful to pause rendering while minimized.
        void Set_iconify_callback(Iconify_callback _callback);

        // Called whenever the window's position on screen changes.
        // Useful to keep a secondary window anchored relative to this one
        // (e.g. an inspector panel that follows the main window).
        void Set_position_callback(Position_callback _callback);

    private:
        // Internal GLFW callbacks. Must be static/free functions because
        // GLFW's C API doesn't support member function pointers - they
        // retrieve the owning Window instance via glfwGetWindowUserPointer.
        static void Framebuffer_resize_callback(GLFWwindow* _glfw_window, int _width, int _height);
        static void Window_close_callback(GLFWwindow* _glfw_window);
        static void Window_focus_callback(GLFWwindow* _glfw_window, int _focused);
        static void Window_iconify_callback(GLFWwindow* _glfw_window, int _iconified);
        static void Window_position_callback(GLFWwindow* _glfw_window, int _x, int _y);
       

        Close_callback close_callback;
        Focus_callback focus_callback;
        Iconify_callback iconify_callback;
        Position_callback position_callback;

        
    };

}