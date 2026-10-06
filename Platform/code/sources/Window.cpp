#include "Window.hpp"

// No graphics API header is needed here: the window is created without a
// client API and Vulkan surfaces are created by the Renderer.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdexcept>
#include <iostream>
#include <cassert>
#include <string>
namespace Platform {

    // Window::no_size_limit is declared without including GLFW; this is the
    // one place where the two values meet.
    static_assert(Window::no_size_limit == GLFW_DONT_CARE,
        "Window::no_size_limit must match GLFW_DONT_CARE");

    // ---------- GLFW error callback ----------
    static void Glfw_error_callback(int _error_code, const char* _description) {
        std::cerr << "[GLFW Error " << _error_code << "] " << _description << "\n";
    }

    // ---------- GLFW library lifetime ----------

    // GLFW is initialized by the first Window and terminated when the last
    // one goes away. A "reference" is held by every Window that owns a GLFW
    // window and, for as long as the constructor runs, by the Window being
    // built. Counting the one being built is what makes a constructor that
    // fails (the destructor of a half-built object never runs) give the
    // reference back instead of leaving GLFW initialized for good.
    //
    // Not thread-safe, on purpose: GLFW itself has to be driven from the main
    // thread.
    namespace {
        int glfw_reference_count = 0;

        // The text of the last GLFW error on this thread, appended to
        // _what, so a failure that throws says why and not just what.
        std::string Describe_glfw_failure(const char* _what) {
            std::string message = _what;

            const char* description = nullptr;
            if (glfwGetError(&description) != GLFW_NO_ERROR && description != nullptr) {
                message += ": ";
                message += description;
            }
            return message;
        }

        // Throws std::runtime_error if GLFW cannot be initialized, holding
        // no reference in that case.
        void Acquire_glfw() {
            if (glfw_reference_count == 0) {
                glfwSetErrorCallback(Glfw_error_callback);
                if (!glfwInit()) {
                    throw std::runtime_error(Describe_glfw_failure("Failed to initialize GLFW"));
                }
            }
            ++glfw_reference_count;
        }

        void Release_glfw() noexcept {
            assert(glfw_reference_count > 0 && "Release_glfw() without a matching Acquire_glfw()");

            --glfw_reference_count;
            if (glfw_reference_count == 0) {
                glfwTerminate();
            }
        }

        // Holds one reference to GLFW until Dismiss() hands it over to
        // whoever keeps it from then on (here: the Window, which releases it
        // in Destroy_native()).
        class Glfw_reference
        {
        public:
            Glfw_reference() { Acquire_glfw(); }
            ~Glfw_reference() { if (held) Release_glfw(); }

            Glfw_reference(const Glfw_reference&) = delete;
            Glfw_reference& operator=(const Glfw_reference&) = delete;

            void Dismiss() noexcept { held = false; }

        private:
            bool held = true;
        };
    }

    // ---------- File-local helpers ----------

    // The Window that owns a GLFW window, or nullptr if none is attached.
    static Window* Get_owner(GLFWwindow* _glfw_window) {
        return static_cast<Window*>(glfwGetWindowUserPointer(_glfw_window));
    }

    // Primary monitor and its current video mode. Converts to false when the
    // system exposes no usable monitor (headless, remote session...), so each
    // caller only decides how to report it.
    namespace {
        struct Primary_monitor {
            GLFWmonitor*       monitor = nullptr;
            const GLFWvidmode* mode = nullptr;

            explicit operator bool() const { return monitor != nullptr && mode != nullptr; }
        };
    }

    static Primary_monitor Get_primary_monitor() {
        Primary_monitor primary;
        primary.monitor = glfwGetPrimaryMonitor();
        if (primary.monitor) {
            primary.mode = glfwGetVideoMode(primary.monitor);
        }
        return primary;
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

        // Any exit from the constructor before Dismiss() below releases this
        // reference, terminating GLFW again if this was the first Window.
        Glfw_reference glfw_reference;

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
            throw std::runtime_error(Describe_glfw_failure("Failed to create GLFW window"));
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

        // From here the Window owns the reference: Destroy_native() releases it.
        glfw_reference.Dismiss();
    }

    // ---------- Destructor ----------
    Window::~Window() {
        Destroy_native();
    }

    // ---------- Destroy_native ----------
    void Window::Destroy_native() {
        if (!window_handle) return;

        glfwDestroyWindow(window_handle);
        window_handle = nullptr;

        Release_glfw();
    }

    // ---------- Move_from ----------
    void Window::Move_from(Window&& _other) noexcept {
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
        min_width = _other.min_width;
        min_height = _other.min_height;
        max_width = _other.max_width;
        max_height = _other.max_height;

        // Every callback travels with the window. Leaving the input ones
        // behind would silently disconnect Input from a moved Window.
        close_callback = std::move(_other.close_callback);
        focus_callback = std::move(_other.focus_callback);
        iconify_callback = std::move(_other.iconify_callback);
        position_callback = std::move(_other.position_callback);
        key_callback = std::move(_other.key_callback);
        mouse_button_callback = std::move(_other.mouse_button_callback);
        mouse_move_callback = std::move(_other.mouse_move_callback);
        scroll_callback = std::move(_other.scroll_callback);

        _other.window_handle = nullptr;
        _other.close_callback = nullptr;
        _other.focus_callback = nullptr;
        _other.iconify_callback = nullptr;
        _other.position_callback = nullptr;
        _other.key_callback = nullptr;
        _other.mouse_button_callback = nullptr;
        _other.mouse_move_callback = nullptr;
        _other.scroll_callback = nullptr;

        if (window_handle) {
            glfwSetWindowUserPointer(window_handle, this);
        }
    }

    // ---------- Move constructor ----------
    Window::Window(Window&& _other) noexcept
        : window_handle(nullptr),
        width(0),
        height(0),
        was_resized(false),
        windowed_pos_x(0),
        windowed_pos_y(0),
        windowed_width(0),
        windowed_height(0),
        is_fullscreen(false),
        is_windowed_fullscreen(false) {

        Move_from(std::move(_other));
    }

    // ---------- Move assignment ----------
    Window& Window::operator=(Window&& _other) noexcept {
        if (this != &_other) {
            Destroy_native();

            Move_from(std::move(_other));
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
    void Window::Wait_events() {
        assert(window_handle != nullptr && "Wait_events() called on a moved-from Window");
        glfwWaitEvents();
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

    // glfwSetWindowSizeLimits takes the minimum AND the maximum in one call,
    // and a GLFW_DONT_CARE argument means "remove that limit", not "leave it
    // alone". So every change is made by re-sending all four values: the new
    // ones plus the stored ones for the limit the caller did not touch.
    bool Window::Apply_size_limits(int _min_width, int _min_height, int _max_width, int _max_height) {
        // GLFW rejects an inconsistent set with GLFW_INVALID_VALUE and applies
        // nothing, which the members would then misreport. Checking first
        // keeps them equal to what is in effect.
        const bool width_inconsistent = _min_width != no_size_limit && _max_width != no_size_limit
            && _min_width > _max_width;
        const bool height_inconsistent = _min_height != no_size_limit && _max_height != no_size_limit
            && _min_height > _max_height;

        if (width_inconsistent || height_inconsistent) {
            std::cerr << "[Window] Size limits ignored: the minimum (" << _min_width << "x" << _min_height
                << ") exceeds the maximum (" << _max_width << "x" << _max_height << ")\n";
            return false;
        }

        glfwSetWindowSizeLimits(window_handle, _min_width, _min_height, _max_width, _max_height);

        min_width = _min_width;
        min_height = _min_height;
        max_width = _max_width;
        max_height = _max_height;
        return true;
    }

    bool Window::Set_min_size(int _min_width, int _min_height) {
        assert(window_handle != nullptr && "Set_min_size() called on a moved-from Window");
        assert(_min_width >= 0 && _min_height >= 0 && "Min size cannot be negative");

        // Also checked in release builds: -1 would silently mean "no limit",
        // and any other negative value is a GLFW error.
        if (_min_width < 0 || _min_height < 0) {
            std::cerr << "[Window] Set_min_size ignored: negative size\n";
            return false;
        }

        return Apply_size_limits(_min_width, _min_height, max_width, max_height);
    }

    bool Window::Set_max_size(int _max_width, int _max_height) {
        assert(window_handle != nullptr && "Set_max_size() called on a moved-from Window");
        assert(_max_width >= 0 && _max_height >= 0 && "Max size cannot be negative");

        if (_max_width < 0 || _max_height < 0) {
            std::cerr << "[Window] Set_max_size ignored: negative size\n";
            return false;
        }

        return Apply_size_limits(min_width, min_height, _max_width, _max_height);
    }

    void Window::Clear_size_limits() {
        assert(window_handle != nullptr && "Clear_size_limits() called on a moved-from Window");
        Apply_size_limits(no_size_limit, no_size_limit, no_size_limit, no_size_limit);
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

        const Primary_monitor primary = Get_primary_monitor();
        if (!primary) return;

        int window_width, window_height;
        Get_size(window_width, window_height);

        int x = (primary.mode->width - window_width) / 2;
        int y = (primary.mode->height - window_height) / 2;

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

    void Window::Remember_windowed_geometry() {
        if (is_fullscreen || is_windowed_fullscreen) return;

        glfwGetWindowPos(window_handle, &windowed_pos_x, &windowed_pos_y);
        Get_size(windowed_width, windowed_height);
    }

    void Window::Restore_windowed_geometry() {
        Set_decorated(true);
        glfwSetWindowMonitor(
            window_handle, nullptr,
            windowed_pos_x, windowed_pos_y,
            windowed_width, windowed_height,
            0
        );
    }

    void Window::Set_fullscreen(bool _fullscreen) {
        assert(window_handle != nullptr && "Set_fullscreen() called on a moved-from Window");

        if (_fullscreen == is_fullscreen) return;

        if (_fullscreen) {
            const Primary_monitor primary = Get_primary_monitor();
            if (!primary) {
                std::cerr << "[Window] Set_fullscreen: no primary monitor available\n";
                return;
            }

            // A no-op when coming from borderless: the geometry saved when
            // the window was last windowed is the one to restore later.
            Remember_windowed_geometry();

            glfwSetWindowMonitor(
                window_handle, primary.monitor,
                0, 0,
                primary.mode->width, primary.mode->height,
                primary.mode->refreshRate
            );

            is_fullscreen = true;
            is_windowed_fullscreen = false;
        }
        else {
            // Restores decorations too, in case the path went
            // windowed -> borderless -> fullscreen -> windowed.
            Restore_windowed_geometry();

            is_fullscreen = false;
            is_windowed_fullscreen = false;
        }
    }

    bool Window::Is_fullscreen() const {
        assert(window_handle != nullptr && "Is_fullscreen() called on a moved-from Window");
        return is_fullscreen;
    }

    void Window::Set_windowed_fullscreen(bool _enabled) {
        assert(window_handle != nullptr && "Set_windowed_fullscreen() called on a moved-from Window");

        if (_enabled == is_windowed_fullscreen) return;

        if (_enabled) {
            const Primary_monitor primary = Get_primary_monitor();
            if (!primary) {
                std::cerr << "[Window] Set_windowed_fullscreen: no primary monitor available\n";
                return;
            }

            Remember_windowed_geometry();

            Set_decorated(false);
            glfwSetWindowMonitor(
                window_handle, nullptr,
                0, 0,
                primary.mode->width, primary.mode->height,
                0
            );

            is_windowed_fullscreen = true;
            is_fullscreen = false;
        }
        else {
            Restore_windowed_geometry();

            is_windowed_fullscreen = false;
            is_fullscreen = false;
        }
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
        Window* owner = Get_owner(_glfw_window);
        if (owner) {
            owner->width = static_cast<uint32_t>(_width);
            owner->height = static_cast<uint32_t>(_height);
            owner->was_resized = true;
        }
    }

    void Window::Window_close_callback(GLFWwindow* _glfw_window) {
        Window* owner = Get_owner(_glfw_window);
        if (owner && owner->close_callback) {
            owner->close_callback();
        }
    }

    void Window::Window_focus_callback(GLFWwindow* _glfw_window, int _focused) {
        Window* owner = Get_owner(_glfw_window);
        if (owner && owner->focus_callback) {
            owner->focus_callback(_focused == GLFW_TRUE);
        }
    }

    void Window::Window_iconify_callback(GLFWwindow* _glfw_window, int _iconified) {
        Window* owner = Get_owner(_glfw_window);
        if (owner && owner->iconify_callback) {
            owner->iconify_callback(_iconified == GLFW_TRUE);
        }
    }

    void Window::Window_position_callback(GLFWwindow* _glfw_window, int _x, int _y) {
        Window* owner = Get_owner(_glfw_window);
        if (owner && owner->position_callback) {
            owner->position_callback(_x, _y);
        }
    }
    void Window::Key_callback_internal(GLFWwindow* _w, int _key, int /*_scancode*/, int _action, int /*_mods*/)
    {
        Window* owner = Get_owner(_w);
        if (owner && owner->key_callback && _action != GLFW_REPEAT)
            owner->key_callback(_key, _action);
    }

    void Window::Mouse_button_callback_internal(GLFWwindow* _w, int _button, int _action, int /*_mods*/)
    {
        Window* owner = Get_owner(_w);
        if (owner && owner->mouse_button_callback)
            owner->mouse_button_callback(_button, _action);
    }

    void Window::Mouse_move_callback_internal(GLFWwindow* _w, double _xpos, double _ypos)
    {
        Window* owner = Get_owner(_w);
        if (owner && owner->mouse_move_callback)
            owner->mouse_move_callback(_xpos, _ypos);
    }

    void Window::Scroll_callback_internal(GLFWwindow* _w, double /*_xoffset*/, double _yoffset)
    {
        Window* owner = Get_owner(_w);
        if (owner && owner->scroll_callback)
            owner->scroll_callback(_yoffset);
    }

}