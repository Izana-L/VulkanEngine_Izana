#include "Window.hpp"
#include <stdexcept>
#include <iostream>
#include <cassert>
namespace Platform {

    int Window::window_count = 0;

    // ---------- GLFW error callback ----------
    static void Glfw_error_callback(int _error_code, const char* _description) {
        std::cerr << "[GLFW Error " << _error_code << "] " << _description << "\n";
    }

    // ---------- Constructor ----------
    Window::Window(uint32_t _width, uint32_t _height, const std::string& _title)
        : window_handle(nullptr),
        width(_width),
        height(_height),
        title(_title),
        was_resized(false),
        windowed_pos_x(0),
        windowed_pos_y(0),
        windowed_width(0),
        windowed_height(0),
        is_fullscreen(false),
        is_windowed_fullscreen(false) {

        // Dimensiones de 0 indican un error de programacion del que llama
        // (alguien paso mal los parametros), no una condicion del entorno
        assert(_width > 0 && _height > 0 && "Window dimensions must be greater than zero");

        if (window_count == 0) {
            glfwSetErrorCallback(Glfw_error_callback);
            if (!glfwInit()) {
                throw std::runtime_error("Failed to initialize GLFW");
            }
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        window_handle = glfwCreateWindow(
            static_cast<int>(_width),
            static_cast<int>(_height),
            _title.c_str(),
            nullptr,
            nullptr
        );

        if (!window_handle) {
            throw std::runtime_error("Failed to create GLFW window");
        }

        glfwSetWindowUserPointer(window_handle, this);

        glfwSetFramebufferSizeCallback(window_handle, Framebuffer_resize_callback);
        glfwSetWindowCloseCallback(window_handle, Window_close_callback);
        glfwSetWindowFocusCallback(window_handle, Window_focus_callback);
        glfwSetWindowIconifyCallback(window_handle, Window_iconify_callback);
        glfwSetWindowPosCallback(window_handle, Window_position_callback);
        glfwSetKeyCallback(window_handle, Key_callback_internal);
        glfwSetMouseButtonCallback(window_handle, Mouse_button_callback_internal);
        glfwSetCursorPosCallback(window_handle, Mouse_move_callback_internal);
        glfwSetScrollCallback(window_handle, Scroll_callback_internal);
        glfwGetWindowPos(window_handle, &windowed_pos_x, &windowed_pos_y);
        windowed_width = static_cast<int>(_width);
        windowed_height = static_cast<int>(_height);

        ++window_count;
    }

    // ---------- Destructor ----------
    Window::~Window() {
        if (window_handle) {
            glfwDestroyWindow(window_handle);
            --window_count;
            if (window_count == 0) {
                glfwTerminate();
            }
        }
    }

    // ---------- Move constructor ----------
    Window::Window(Window&& _other) noexcept
        : window_handle(_other.window_handle),
        width(_other.width),
        height(_other.height),
        title(std::move(_other.title)),
        was_resized(_other.was_resized),
        windowed_pos_x(_other.windowed_pos_x),
        windowed_pos_y(_other.windowed_pos_y),
        windowed_width(_other.windowed_width),
        windowed_height(_other.windowed_height),
        is_fullscreen(_other.is_fullscreen),
        is_windowed_fullscreen(_other.is_windowed_fullscreen),
        close_callback(std::move(_other.close_callback)),
        focus_callback(std::move(_other.focus_callback)),
        iconify_callback(std::move(_other.iconify_callback)),
        position_callback(std::move(_other.position_callback)) {

        _other.window_handle = nullptr;

        if (window_handle) {
            glfwSetWindowUserPointer(window_handle, this);
        }
    }

    // ---------- Move assignment ----------
    Window& Window::operator=(Window&& _other) noexcept {
        if (this != &_other) {
            if (window_handle) {
                glfwDestroyWindow(window_handle);
                --window_count;
                if (window_count == 0) {
                    glfwTerminate();
                }
            }

            window_handle = _other.window_handle;
            width = _other.width;
            height = _other.height;
            title = std::move(_other.title);
            was_resized = _other.was_resized;
            windowed_pos_x = _other.windowed_pos_x;
            windowed_pos_y = _other.windowed_pos_y;
            windowed_width = _other.windowed_width;
            windowed_height = _other.windowed_height;
            is_fullscreen = _other.is_fullscreen;
            is_windowed_fullscreen = _other.is_windowed_fullscreen;
            close_callback = std::move(_other.close_callback);
            focus_callback = std::move(_other.focus_callback);
            iconify_callback = std::move(_other.iconify_callback);
            position_callback = std::move(_other.position_callback);

            _other.window_handle = nullptr;

            if (window_handle) {
                glfwSetWindowUserPointer(window_handle, this);
            }
        }
        return *this;
    }

    // =========================================================
    // Core loop
    // =========================================================

    bool Window::Should_close() const {
        assert(window_handle != nullptr && "Should_close() called on a moved-from Window");
        return glfwWindowShouldClose(window_handle) != 0;
    }

    void Window::Poll_events() {
        assert(window_handle != nullptr && "Poll_events() called on a moved-from Window");
        glfwPollEvents();
    }

    // =========================================================
    // Size
    // =========================================================

    void Window::Get_size(int& _out_width, int& _out_height) const {
        assert(window_handle != nullptr && "Get_size() called on a moved-from Window");
        glfwGetWindowSize(window_handle, &_out_width, &_out_height);
    }

    void Window::Get_framebuffer_size(int& _out_width, int& _out_height) const {
        assert(window_handle != nullptr && "Get_framebuffer_size() called on a moved-from Window");
        glfwGetFramebufferSize(window_handle, &_out_width, &_out_height);
    }

    bool Window::Consume_resized_flag() {
        assert(window_handle != nullptr && "Consume_resized_flag() called on a moved-from Window");
        bool result = was_resized;
        was_resized = false;
        return result;
    }

    void Window::Set_min_size(int _min_width, int _min_height) {
        assert(window_handle != nullptr && "Set_min_size() called on a moved-from Window");
        assert(_min_width >= 0 && _min_height >= 0 && "Min size cannot be negative");
        glfwSetWindowSizeLimits(window_handle, _min_width, _min_height, GLFW_DONT_CARE, GLFW_DONT_CARE);
    }

    void Window::Set_max_size(int _max_width, int _max_height) {
        assert(window_handle != nullptr && "Set_max_size() called on a moved-from Window");
        assert(_max_width >= 0 && _max_height >= 0 && "Max size cannot be negative");
        glfwSetWindowSizeLimits(window_handle, GLFW_DONT_CARE, GLFW_DONT_CARE, _max_width, _max_height);
    }

    void Window::Set_resizable(bool _resizable) {
        assert(window_handle != nullptr && "Set_resizable() called on a moved-from Window");
        glfwSetWindowAttrib(window_handle, GLFW_RESIZABLE, _resizable ? GLFW_TRUE : GLFW_FALSE);
    }

    // =========================================================
    // Title and icon
    // =========================================================

    void Window::Set_title(const std::string& _title) {
        assert(window_handle != nullptr && "Set_title() called on a moved-from Window");
        title = _title;
        glfwSetWindowTitle(window_handle, _title.c_str());
    }

    const std::string& Window::Get_title() const {
        assert(window_handle != nullptr && "Get_title() called on a moved-from Window");
        return title;
    }

    void Window::Set_icon(const unsigned char* _pixels, int _width, int _height) {
        assert(window_handle != nullptr && "Set_icon() called on a moved-from Window");
        assert(_pixels != nullptr && "Icon pixel data cannot be null");
        assert(_width > 0 && _height > 0 && "Icon dimensions must be greater than zero");

        GLFWimage image;
        image.width = _width;
        image.height = _height;
        image.pixels = const_cast<unsigned char*>(_pixels);

        glfwSetWindowIcon(window_handle, 1, &image);
    }

    // =========================================================
    // Position
    // =========================================================

    void Window::Set_position(int _x, int _y) {
        assert(window_handle != nullptr && "Set_position() called on a moved-from Window");
        glfwSetWindowPos(window_handle, _x, _y);
    }

    void Window::Get_position(int& _out_x, int& _out_y) const {
        assert(window_handle != nullptr && "Get_position() called on a moved-from Window");
        glfwGetWindowPos(window_handle, &_out_x, &_out_y);
    }

    void Window::Center() {
        assert(window_handle != nullptr && "Center() called on a moved-from Window");

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        if (!monitor) return;

        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (!mode) return;

        int window_width, window_height;
        Get_size(window_width, window_height);

        int x = (mode->width - window_width) / 2;
        int y = (mode->height - window_height) / 2;

        glfwSetWindowPos(window_handle, x, y);
    }

    // =========================================================
    // Window state
    // =========================================================

    void Window::Minimize() {
        assert(window_handle != nullptr && "Minimize() called on a moved-from Window");
        glfwIconifyWindow(window_handle);
    }

    void Window::Maximize() {
        assert(window_handle != nullptr && "Maximize() called on a moved-from Window");
        glfwMaximizeWindow(window_handle);
    }

    void Window::Restore() {
        assert(window_handle != nullptr && "Restore() called on a moved-from Window");
        glfwRestoreWindow(window_handle);
    }

    void Window::Set_decorated(bool _decorated) {
        assert(window_handle != nullptr && "Set_decorated() called on a moved-from Window");
        glfwSetWindowAttrib(window_handle, GLFW_DECORATED, _decorated ? GLFW_TRUE : GLFW_FALSE);
    }

    bool Window::Is_decorated() const {
        assert(window_handle != nullptr && "Is_decorated() called on a moved-from Window");
        return glfwGetWindowAttrib(window_handle, GLFW_DECORATED) == GLFW_TRUE;
    }

    bool Window::Is_minimized() const {
        assert(window_handle != nullptr && "Is_minimized() called on a moved-from Window");
        int w, h;
        Get_framebuffer_size(w, h);
        return w == 0 || h == 0;
    }

    bool Window::Is_maximized() const {
        assert(window_handle != nullptr && "Is_maximized() called on a moved-from Window");
        return glfwGetWindowAttrib(window_handle, GLFW_MAXIMIZED) == GLFW_TRUE;
    }

    bool Window::Is_focused() const {
        assert(window_handle != nullptr && "Is_focused() called on a moved-from Window");
        return glfwGetWindowAttrib(window_handle, GLFW_FOCUSED) == GLFW_TRUE;
    }

    bool Window::Is_visible() const {
        assert(window_handle != nullptr && "Is_visible() called on a moved-from Window");
        return glfwGetWindowAttrib(window_handle, GLFW_VISIBLE) == GLFW_TRUE;
    }

    void Window::Focus() {
        assert(window_handle != nullptr && "Focus() called on a moved-from Window");
        glfwFocusWindow(window_handle);
    }

    void Window::Show() {
        assert(window_handle != nullptr && "Show() called on a moved-from Window");
        glfwShowWindow(window_handle);
    }

    void Window::Hide() {
        assert(window_handle != nullptr && "Hide() called on a moved-from Window");
        glfwHideWindow(window_handle);
    }

    // =========================================================
    // Fullscreen
    // =========================================================

    void Window::Set_fullscreen(bool _fullscreen) {
        assert(window_handle != nullptr && "Set_fullscreen() called on a moved-from Window");

        if (_fullscreen == is_fullscreen) return;

        if (_fullscreen) {
            glfwGetWindowPos(window_handle, &windowed_pos_x, &windowed_pos_y);
            Get_size(windowed_width, windowed_height);

            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);

            glfwSetWindowMonitor(
                window_handle, monitor,
                0, 0,
                mode->width, mode->height,
                mode->refreshRate
            );
        }
        else {
            glfwSetWindowMonitor(
                window_handle, nullptr,
                windowed_pos_x, windowed_pos_y,
                windowed_width, windowed_height,
                0
            );
        }

        is_fullscreen = _fullscreen;
        is_windowed_fullscreen = false;
    }

    bool Window::Is_fullscreen() const {
        assert(window_handle != nullptr && "Is_fullscreen() called on a moved-from Window");
        return is_fullscreen;
    }

    void Window::Set_windowed_fullscreen(bool _enabled) {
        assert(window_handle != nullptr && "Set_windowed_fullscreen() called on a moved-from Window");

        if (_enabled == is_windowed_fullscreen) return;

        if (_enabled) {
            glfwGetWindowPos(window_handle, &windowed_pos_x, &windowed_pos_y);
            Get_size(windowed_width, windowed_height);

            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);

            Set_decorated(false);
            glfwSetWindowMonitor(
                window_handle, nullptr,
                0, 0,
                mode->width, mode->height,
                0
            );
        }
        else {
            Set_decorated(true);
            glfwSetWindowMonitor(
                window_handle, nullptr,
                windowed_pos_x, windowed_pos_y,
                windowed_width, windowed_height,
                0
            );
        }

        is_windowed_fullscreen = _enabled;
        is_fullscreen = false;
    }

    bool Window::Is_windowed_fullscreen() const {
        assert(window_handle != nullptr && "Is_windowed_fullscreen() called on a moved-from Window");
        return is_windowed_fullscreen;
    }

    // =========================================================
    // Monitor / DPI
    // =========================================================

    void Window::Get_content_scale(float& _out_scale_x, float& _out_scale_y) const {
        assert(window_handle != nullptr && "Get_content_scale() called on a moved-from Window");
        glfwGetWindowContentScale(window_handle, &_out_scale_x, &_out_scale_y);
    }

    int Window::Get_monitor_count() {
        int count = 0;
        glfwGetMonitors(&count);
        return count;
    }

    // =========================================================
    // Native access
    // =========================================================

    GLFWwindow* Window::Get_native_handle() const {
        assert(window_handle != nullptr && "Get_native_handle() called on a moved-from Window");
        return window_handle;
    }

    // =========================================================
    // Callbacks (public setters)
    // =========================================================

    void Window::Set_close_callback(Close_callback _callback) {
        assert(window_handle != nullptr && "Set_close_callback() called on a moved-from Window");
        close_callback = std::move(_callback);
    }

    void Window::Set_focus_callback(Focus_callback _callback) {
        assert(window_handle != nullptr && "Set_focus_callback() called on a moved-from Window");
        focus_callback = std::move(_callback);
    }

    void Window::Set_iconify_callback(Iconify_callback _callback) {
        assert(window_handle != nullptr && "Set_iconify_callback() called on a moved-from Window");
        iconify_callback = std::move(_callback);
    }
    
    void Window::Set_position_callback(Position_callback _callback) {
        assert(window_handle != nullptr && "Set_position_callback() called on a moved-from Window");
        position_callback = std::move(_callback);
    }
    void Window::Set_key_callback(Key_callback _callback)
    {
        assert(window_handle != nullptr && "Set_key_callback() called on a moved-from Window");
        key_callback = std::move(_callback);
    }

    void Window::Set_mouse_button_callback(Mouse_button_callback _callback)
    {
        assert(window_handle != nullptr && "Set_mouse_button_callback() called on a moved-from Window");
        mouse_button_callback = std::move(_callback);
    }

    void Window::Set_mouse_move_callback(Mouse_move_callback _callback)
    {
        assert(window_handle != nullptr && "Set_mouse_move_callback() called on a moved-from Window");
        mouse_move_callback = std::move(_callback);
    }

    void Window::Set_scroll_callback(Scroll_callback _callback)
    {
        assert(window_handle != nullptr && "Set_scroll_callback() called on a moved-from Window");
        scroll_callback = std::move(_callback);
    }

    // =========================================================
    // Internal GLFW callbacks
    // =========================================================

    void Window::Framebuffer_resize_callback(GLFWwindow* _glfw_window, int _width, int _height) {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_glfw_window));
        if (owner) {
            owner->width = static_cast<uint32_t>(_width);
            owner->height = static_cast<uint32_t>(_height);
            owner->was_resized = true;
        }
    }

    void Window::Window_close_callback(GLFWwindow* _glfw_window) {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_glfw_window));
        if (owner && owner->close_callback) {
            owner->close_callback();
        }
    }

    void Window::Window_focus_callback(GLFWwindow* _glfw_window, int _focused) {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_glfw_window));
        if (owner && owner->focus_callback) {
            owner->focus_callback(_focused == GLFW_TRUE);
        }
    }

    void Window::Window_iconify_callback(GLFWwindow* _glfw_window, int _iconified) {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_glfw_window));
        if (owner && owner->iconify_callback) {
            owner->iconify_callback(_iconified == GLFW_TRUE);
        }
    }

    void Window::Window_position_callback(GLFWwindow* _glfw_window, int _x, int _y) {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_glfw_window));
        if (owner && owner->position_callback) {
            owner->position_callback(_x, _y);
        }
    }
    void Window::Key_callback_internal(GLFWwindow* _w, int _key, int /*_scancode*/, int _action, int /*_mods*/)
    {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_w));
        if (owner && owner->key_callback && _action != GLFW_REPEAT)
            owner->key_callback(_key, _action);
    }

    void Window::Mouse_button_callback_internal(GLFWwindow* _w, int _button, int _action, int /*_mods*/)
    {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_w));
        if (owner && owner->mouse_button_callback)
            owner->mouse_button_callback(_button, _action);
    }

    void Window::Mouse_move_callback_internal(GLFWwindow* _w, double _xpos, double _ypos)
    {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_w));
        if (owner && owner->mouse_move_callback)
            owner->mouse_move_callback(_xpos, _ypos);
    }

    void Window::Scroll_callback_internal(GLFWwindow* _w, double /*_xoffset*/, double _yoffset)
    {
        Window* owner = reinterpret_cast<Window*>(glfwGetWindowUserPointer(_w));
        if (owner && owner->scroll_callback)
            owner->scroll_callback(_yoffset);
    }

}