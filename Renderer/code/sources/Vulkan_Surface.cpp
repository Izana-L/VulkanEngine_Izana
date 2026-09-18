#include <Vulkan_Surface.hpp>
#include <Vulkan_Utils.hpp>
#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer_System {

    // ---------- Constructor ----------
    Vulkan_Surface::Vulkan_Surface(const Vulkan_Instance& _instance, const Platform::Window& _window)
                                 : instance_handle(_instance.Get_handle()),surface(VK_NULL_HANDLE)    
    {
        assert(instance_handle != VK_NULL_HANDLE && "Vulkan_Instance must be fully constructed before creating a surface");
        // glfwCreateWindowSurface handles all the platform-specific surface
        // creation internally (on Windows it would otherwise be
        // vkCreateWin32SurfaceKHR with a HWND/HINSTANCE) - GLFW abstracts
        // that away so we don't need any #ifdef _WIN32 here.
        VkResult result = glfwCreateWindowSurface(instance_handle,_window.Get_native_handle(),nullptr,  &surface );
          

        if (result != VK_SUCCESS) 
        {
            throw std::runtime_error("Failed to create window surface: " + Vulkan_Utils::Vk_result_to_string(result));       
        }

        std::cout << "[Vulkan_surface] Surface created successfully.\n";
    }

    // ---------- Destructor ----------
    Vulkan_Surface::~Vulkan_Surface() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Surface::Destroy() {
        if (surface != VK_NULL_HANDLE) 
        {
            vkDestroySurfaceKHR(instance_handle, surface, nullptr);
        }
    }

    // ---------- Move constructor ----------
    Vulkan_Surface::Vulkan_Surface(Vulkan_Surface&& _other) noexcept
        : instance_handle(_other.instance_handle),
        surface(_other.surface) {

        _other.surface = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Surface& Vulkan_Surface::operator=(Vulkan_Surface&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            instance_handle = _other.instance_handle;
            surface = _other.surface;

            _other.surface = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Get_handle ----------
    VkSurfaceKHR Vulkan_Surface::Get_handle() const {
        return surface;
    }

}