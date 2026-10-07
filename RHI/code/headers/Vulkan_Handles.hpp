#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Vulkan_Image_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <cstdint>
#include <utility>

namespace Renderer_System
{

    // Vulkan_Handles: owning wrappers for single Vulkan handles.
    //
    // Each wrapper holds the handle and what its destruction needs, and
    // destroys the handle when it goes out of scope. They are move-only.
    // Members of these types make a constructor exception safe without a
    // try/catch: when the creation of member N throws, the members 1..N-1
    // are already constructed and the language destroys them, in reverse
    // order.
    //
    // Ownership is taken after a successful creation only. The creation
    // functions of the engine return a raw handle once VK_SUCCESS was
    // checked, and the wrapper is constructed from that value, so the value
    // of an output parameter of a failed vkCreate* call (undefined by the
    // specification) never reaches a wrapper.
    //
    // The wrappers are written for the C API and VMA the engine uses; they
    // do not depend on Vulkan-Hpp.

    // =========================================================
    // Handles destroyed through a VkDevice
    // =========================================================

    // HANDLE: the Vulkan handle type. DELETER: a type with a static
    // Destroy(VkDevice, HANDLE) that destroys it.
    template <typename HANDLE, typename DELETER>
    class Unique_Device_Handle
    {
    public:

        Unique_Device_Handle() noexcept = default;

        // Takes ownership of _handle, which must have been created
        // successfully on _device (or be VK_NULL_HANDLE).
        Unique_Device_Handle(VkDevice _device, HANDLE _handle) noexcept
            : device(_device), handle(_handle)
        {
        }

        ~Unique_Device_Handle()
        {
            Reset();
        }

        Unique_Device_Handle(const Unique_Device_Handle&) = delete;
        Unique_Device_Handle& operator=(const Unique_Device_Handle&) = delete;

        Unique_Device_Handle(Unique_Device_Handle&& _other) noexcept
            : device(_other.device), handle(_other.Release())
        {
        }

        Unique_Device_Handle& operator=(Unique_Device_Handle&& _other) noexcept
        {
            if (this != &_other)
            {
                Reset();

                device = _other.device;
                handle = _other.Release();
            }

            return *this;
        }

        // Destroys the handle, if any. The wrapper is empty afterwards.
        void Reset() noexcept
        {
            if (handle != VK_NULL_HANDLE)
            {
                DELETER::Destroy(device, handle);
                handle = VK_NULL_HANDLE;
            }
        }

        // Gives up ownership without destroying the handle.
        HANDLE Release() noexcept
        {
            const HANDLE released = handle;
            handle = VK_NULL_HANDLE;
            return released;
        }

        HANDLE Get() const noexcept { return handle; }

        explicit operator bool() const noexcept { return handle != VK_NULL_HANDLE; }

    private:

        VkDevice device = VK_NULL_HANDLE;
        HANDLE   handle = VK_NULL_HANDLE;
    };

    struct Image_View_Deleter
    {
        static void Destroy(VkDevice _device, VkImageView _handle) noexcept { vkDestroyImageView(_device, _handle, nullptr); }
    };

    struct Framebuffer_Deleter
    {
        static void Destroy(VkDevice _device, VkFramebuffer _handle) noexcept { vkDestroyFramebuffer(_device, _handle, nullptr); }
    };

    struct Swapchain_Deleter
    {
        static void Destroy(VkDevice _device, VkSwapchainKHR _handle) noexcept { vkDestroySwapchainKHR(_device, _handle, nullptr); }
    };

    struct Semaphore_Deleter
    {
        static void Destroy(VkDevice _device, VkSemaphore _handle) noexcept { vkDestroySemaphore(_device, _handle, nullptr); }
    };

    struct Fence_Deleter
    {
        static void Destroy(VkDevice _device, VkFence _handle) noexcept { vkDestroyFence(_device, _handle, nullptr); }
    };

    struct Pipeline_Deleter
    {
        static void Destroy(VkDevice _device, VkPipeline _handle) noexcept { vkDestroyPipeline(_device, _handle, nullptr); }
    };

    // Destroying a command pool frees every command buffer allocated from it.
    struct Command_Pool_Deleter
    {
        static void Destroy(VkDevice _device, VkCommandPool _handle) noexcept { vkDestroyCommandPool(_device, _handle, nullptr); }
    };

    struct Render_Pass_Deleter
    {
        static void Destroy(VkDevice _device, VkRenderPass _handle) noexcept { vkDestroyRenderPass(_device, _handle, nullptr); }
    };

    struct Shader_Module_Deleter
    {
        static void Destroy(VkDevice _device, VkShaderModule _handle) noexcept { vkDestroyShaderModule(_device, _handle, nullptr); }
    };

    using Unique_Image_View    = Unique_Device_Handle<VkImageView, Image_View_Deleter>;
    using Unique_Framebuffer   = Unique_Device_Handle<VkFramebuffer, Framebuffer_Deleter>;
    using Unique_Swapchain     = Unique_Device_Handle<VkSwapchainKHR, Swapchain_Deleter>;
    using Unique_Semaphore     = Unique_Device_Handle<VkSemaphore, Semaphore_Deleter>;
    using Unique_Fence         = Unique_Device_Handle<VkFence, Fence_Deleter>;
    using Unique_Pipeline      = Unique_Device_Handle<VkPipeline, Pipeline_Deleter>;
    using Unique_Command_Pool  = Unique_Device_Handle<VkCommandPool, Command_Pool_Deleter>;
    using Unique_Render_Pass   = Unique_Device_Handle<VkRenderPass, Render_Pass_Deleter>;
    using Unique_Shader_Module = Unique_Device_Handle<VkShaderModule, Shader_Module_Deleter>;

    // =========================================================
    // Creation of synchronization objects
    // =========================================================

    // A binary semaphore, unsignaled.
    inline Unique_Semaphore Create_binary_semaphore(VkDevice _device, const char* _what)
    {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkSemaphore semaphore = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSemaphore(_device, &info, nullptr, &semaphore), _what);

        return Unique_Semaphore(_device, semaphore);
    }

    // A timeline semaphore holding _initial_value. Needs the
    // timelineSemaphore feature.
    inline Unique_Semaphore Create_timeline_semaphore(VkDevice _device, uint64_t _initial_value, const char* _what)
    {
        VkSemaphoreTypeCreateInfo type_info{};
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue = _initial_value;

        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &type_info;

        VkSemaphore semaphore = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSemaphore(_device, &info, nullptr, &semaphore), _what);

        return Unique_Semaphore(_device, semaphore);
    }

    // A fence, signaled or not.
    inline Unique_Fence Create_fence(VkDevice _device, bool _signaled, const char* _what)
    {
        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        info.flags = _signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;

        VkFence fence = VK_NULL_HANDLE;
        VK_CHECK(vkCreateFence(_device, &info, nullptr, &fence), _what);

        return Unique_Fence(_device, fence);
    }

    // =========================================================
    // Image views
    // =========================================================

    // A 2D view of _image over the mip levels 0 .. _mip_levels - 1, owned
    // from the moment it exists (Vulkan_Image_Utils::Create_image_view).
    inline Unique_Image_View Create_unique_image_view(VkDevice _device, VkImage _image, VkFormat _format,
                                                      VkImageAspectFlags _aspect_flags, uint32_t _mip_levels)
    {
        return Unique_Image_View(_device, Vulkan_Image_Utils::Create_image_view(_device, _image, _format, _aspect_flags, _mip_levels));
    }

    // =========================================================
    // VMA images
    // =========================================================

    // An image together with its VMA allocation, freed together
    // (Vulkan_Image_Utils::Destroy_image). Image views of the image are
    // separate objects and are destroyed by their own wrappers.
    class Unique_Image
    {
    public:

        Unique_Image() noexcept = default;

        // Takes ownership of _image, created by Vulkan_Image_Utils::
        // Create_image (or empty) on _allocator.
        Unique_Image(VmaAllocator _allocator, Vulkan_Image_Utils::Image_Allocation _image) noexcept
            : allocator(_allocator), image(_image)
        {
        }

        ~Unique_Image()
        {
            Reset();
        }

        Unique_Image(const Unique_Image&) = delete;
        Unique_Image& operator=(const Unique_Image&) = delete;

        Unique_Image(Unique_Image&& _other) noexcept
            : allocator(_other.allocator), image(std::exchange(_other.image, Vulkan_Image_Utils::Image_Allocation{}))
        {
        }

        Unique_Image& operator=(Unique_Image&& _other) noexcept
        {
            if (this != &_other)
            {
                Reset();

                allocator = _other.allocator;
                image = std::exchange(_other.image, Vulkan_Image_Utils::Image_Allocation{});
            }

            return *this;
        }

        // Frees the image and its allocation, if any.
        void Reset() noexcept
        {
            if (image.image != VK_NULL_HANDLE)
                Vulkan_Image_Utils::Destroy_image(allocator, image);
        }

        VkImage Get() const noexcept { return image.image; }

        explicit operator bool() const noexcept { return image.image != VK_NULL_HANDLE; }

    private:

        VmaAllocator                         allocator = VK_NULL_HANDLE;
        Vulkan_Image_Utils::Image_Allocation image{};
    };

} // namespace Renderer_System
