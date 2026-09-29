#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Command_Pool.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Handles.hpp>
#include <Vulkan_Utils.hpp>
#include <Renderer_Limits.hpp>
#include <Gpu_Layouts.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Renderer_System
{

    // The indirect command written by the culling pass and read by the
    // draw: `Draw_Command` in cull_objects.comp mirrors it (five 32-bit
    // values, std430 stride 20). Asserted here, next to the buffers sized
    // by it, because it is the one layout mirror that needs Vulkan.
    static_assert(sizeof(VkDrawIndexedIndirectCommand) == 20, "Draw_Command in cull_objects.comp assumes a 20-byte VkDrawIndexedIndirectCommand");

    // =========================================================
    // Frame_Data
    // =========================================================

    // Frame_Data: the Vulkan resources that must exist independently for
    // each frame-in-flight slot.
    //
    // With FRAMES_IN_FLIGHT = 2, two Frame_Data instances exist:
    // one for the frame the CPU is currently recording, and one for the
    // frame the GPU is currently executing. This overlap is what gives
    // CPU/GPU parallelism without stalling either side.
    //
    // What is NOT here: the semaphore the present operation waits on
    // (render finished) and the present fence. Those belong to the
    // SWAPCHAIN IMAGE, not to the frame slot: a present may still be
    // waiting on them when the slot comes around again, and there are
    // more images than slots. Present_Sync keeps them per image.
    //
    // Nor is the completion of the slot's work: the Renderer tracks the
    // progress of every frame with one timeline semaphore, and a slot is
    // free when the serial of its last submission has been reached.
    //
    // Lifetime: owned by the Renderer, constructed once at startup,
    // destroyed at shutdown. Move-only (owns Vulkan handles).
    //
    // The uniform buffers are mapped persistently at construction time
    // (VMA_ALLOCATION_CREATE_MAPPED_BIT). The Renderer writes into
    // uniform_buffer.mapped_ptr directly each frame with std::memcpy - no
    // map/unmap overhead per frame.
    //
    // Buffers written by the GPU (cluster grid and list, counters, GPU draw
    // commands) are per slot as well: a frame then never writes what the
    // previous frame, possibly still executing, reads, and waiting for the
    // slot's last submission is the only ordering needed against the frame
    // that used the same copy before.
    class Frame_Data
    {
    public:

        Frame_Data(const Vulkan_Device& _device, VmaAllocator _allocator);
        ~Frame_Data();

        Frame_Data(const Frame_Data&) = delete;
        Frame_Data& operator=(const Frame_Data&) = delete;

        Frame_Data(Frame_Data&& _other) noexcept;
        Frame_Data& operator=(Frame_Data&& _other) noexcept;

        // =========================================================
        // Command recording
        // =========================================================

        // One pool per slot so resetting this slot's buffer never touches
        // the buffer the GPU is still executing for the other slot.
        Vulkan_Command_Pool command_pool;

        VkCommandBuffer Get_command_buffer() const
        {
            return command_pool.Get_command_buffer(0);
        }

        // =========================================================
        // Synchronization
        // =========================================================

        // Signaled by the swapchain when the acquired image is ready to be
        // written to. The GPU waits on this before the color attachment
        // output stage. A binary semaphore: the presentation engine does
        // not accept timeline semaphores.
        //
        // It is reusable once the wait of the submission that consumed it
        // has executed, which the wait for the slot's last serial
        // guarantees. That includes the recovery submission of a frame
        // that failed after the acquire (Renderer::Impl::Render).
        Unique_Semaphore image_available_semaphore;

        // =========================================================
        // Buffers written by the CPU (host-visible, persistently mapped)
        // =========================================================

        // Buffer, allocation and the persistent CPU-side pointer, all
        // in one. uniform_buffer.mapped_ptr is valid for the entire
        // lifetime of this Frame_Data.
        Vulkan_Buffer_Utils::Buffer_Allocation uniform_buffer;

        // MAX_LIGHTS entries of Light_GPU, storage buffer.
        Vulkan_Buffer_Utils::Buffer_Allocation light_buffer;

        // MAX_OBJECTS entries of Object_GPU, storage buffer, persistently
        // mapped. Rewritten entirely every frame by the Renderer, one entry
        // per draw; the entry index is the draw's firstInstance.
        Vulkan_Buffer_Utils::Buffer_Allocation object_buffer;

        // MAX_OBJECTS VkDrawIndexedIndirectCommand, INDIRECT_BUFFER usage:
        // the opaque draws when the CPU builds them
        // (Opaque_Draw_Path::Cpu_Indirect), contiguous per pipeline.
        Vulkan_Buffer_Utils::Buffer_Allocation cpu_draw_command_buffer;

        // =========================================================
        // Buffers written by the GPU (device-local)
        // =========================================================

        // CLUSTER_COUNT Cluster_Range_GPU, written by cluster_lights.comp,
        // read by mesh.frag.
        Vulkan_Buffer_Utils::Buffer_Allocation cluster_grid_buffer;

        // CLUSTER_LIGHT_INDEX_CAPACITY light indices, compacted per cluster.
        Vulkan_Buffer_Utils::Buffer_Allocation cluster_light_index_buffer;

        // One Cluster_Counters_GPU: zero-filled (TRANSFER_DST), incremented
        // with atomics, copied for statistics (TRANSFER_SRC).
        Vulkan_Buffer_Utils::Buffer_Allocation cluster_counter_buffer;

        // MAX_OBJECTS VkDrawIndexedIndirectCommand written by
        // cull_objects.comp and consumed by vkCmdDrawIndexedIndirectCount
        // (STORAGE_BUFFER | INDIRECT_BUFFER).
        Vulkan_Buffer_Utils::Buffer_Allocation gpu_draw_command_buffer;

        // One Draw_Count_GPU: zero-filled, incremented by the culling pass,
        // read as the count buffer of the indirect draw and copied for
        // statistics.
        Vulkan_Buffer_Utils::Buffer_Allocation gpu_draw_count_buffer;

        // =========================================================
        // Readback
        // =========================================================

        // One Frame_Stats_GPU, host-visible (Gpu_To_Cpu), filled by transfer
        // copies of the counters and read once the slot's last submission
        // has completed.
        Vulkan_Buffer_Utils::Buffer_Allocation stats_readback_buffer;

    private:

        VkDevice     device_handle;
        VmaAllocator allocator;

        // Every buffer member in one list, so destruction and moves cannot
        // forget one. A new buffer member is added here and BUFFER_COUNT
        // raised.
        static constexpr size_t BUFFER_COUNT = 10;
        std::array<Vulkan_Buffer_Utils::Buffer_Allocation*, BUFFER_COUNT> All_buffers();

        void Destroy();
        void Take_from(Frame_Data& _other) noexcept;
    };

    // ---------- Constructor ----------
    inline Frame_Data::Frame_Data(const Vulkan_Device& _device, VmaAllocator _allocator)
        : command_pool(_device, 1, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT),
        device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator)
    {
        using Vulkan_Buffer_Utils::Create_buffer;
        using Vulkan_Buffer_Utils::Buffer_Access;

        // Anything that throws below leaves the already created buffers to
        // Destroy(), called from the catch block: handles start null, so
        // nothing is destroyed twice. The semaphore is owned by a wrapper
        // and needs no cleanup here.
        try
        {
            image_available_semaphore = Create_binary_semaphore(device_handle,
                "Frame_Data: failed to create image_available semaphore");

            // Persistently mapped, written each frame with std::memcpy.
            uniform_buffer = Create_buffer(allocator, sizeof(Frame_UBO),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Buffer_Access::Cpu_To_Gpu, true);

            light_buffer = Create_buffer(allocator, sizeof(Light_GPU) * MAX_LIGHTS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer_Access::Cpu_To_Gpu, true);

            object_buffer = Create_buffer(allocator, sizeof(Object_GPU) * MAX_OBJECTS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer_Access::Cpu_To_Gpu, true);

            cpu_draw_command_buffer = Create_buffer(allocator, sizeof(VkDrawIndexedIndirectCommand) * MAX_OBJECTS,
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, Buffer_Access::Cpu_To_Gpu, true);

            cluster_grid_buffer = Create_buffer(allocator, sizeof(Cluster_Range_GPU) * CLUSTER_COUNT,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer_Access::Gpu_Only);

            cluster_light_index_buffer = Create_buffer(allocator, sizeof(uint32_t) * CLUSTER_LIGHT_INDEX_CAPACITY,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer_Access::Gpu_Only);

            cluster_counter_buffer = Create_buffer(allocator, sizeof(Cluster_Counters_GPU),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                Buffer_Access::Gpu_Only);

            gpu_draw_command_buffer = Create_buffer(allocator, sizeof(VkDrawIndexedIndirectCommand) * MAX_OBJECTS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, Buffer_Access::Gpu_Only);

            gpu_draw_count_buffer = Create_buffer(allocator, sizeof(Draw_Count_GPU),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                Buffer_Access::Gpu_Only);

            stats_readback_buffer = Create_buffer(allocator, sizeof(Frame_Stats_GPU),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer_Access::Gpu_To_Cpu, true);
        }
        catch (...)
        {
            Destroy();
            throw;
        }
    }

    inline Frame_Data::~Frame_Data()
    {
        Destroy();
    }

    inline std::array<Vulkan_Buffer_Utils::Buffer_Allocation*, Frame_Data::BUFFER_COUNT> Frame_Data::All_buffers()
    {
        return { &uniform_buffer, &light_buffer, &object_buffer, &cpu_draw_command_buffer,
                 &cluster_grid_buffer, &cluster_light_index_buffer, &cluster_counter_buffer,
                 &gpu_draw_command_buffer, &gpu_draw_count_buffer, &stats_readback_buffer };
    }

    inline void Frame_Data::Destroy()
    {
        if (device_handle == VK_NULL_HANDLE) return;

        for (Vulkan_Buffer_Utils::Buffer_Allocation* buffer : All_buffers())
            Vulkan_Buffer_Utils::Destroy_buffer(allocator, *buffer);

        image_available_semaphore.Reset();
    }

    // Steals every handle of _other and leaves it empty (its Destroy() then
    // does nothing). The command pool is moved by the callers.
    inline void Frame_Data::Take_from(Frame_Data& _other) noexcept
    {
        image_available_semaphore = std::move(_other.image_available_semaphore);
        device_handle = _other.device_handle;
        allocator = _other.allocator;

        const auto destination = All_buffers();
        const auto source = _other.All_buffers();

        for (size_t i = 0; i < BUFFER_COUNT; ++i)
        {
            *destination[i] = *source[i];
            *source[i] = {};
        }

        _other.device_handle = VK_NULL_HANDLE;
    }

    inline Frame_Data::Frame_Data(Frame_Data&& _other) noexcept
        : command_pool(std::move(_other.command_pool)),
        device_handle(VK_NULL_HANDLE),
        allocator(VK_NULL_HANDLE)
    {
        Take_from(_other);
    }

    inline Frame_Data& Frame_Data::operator=(Frame_Data&& _other) noexcept
    {
        if (this != &_other)
        {
            Destroy();

            command_pool = std::move(_other.command_pool);
            Take_from(_other);
        }
        return *this;
    }

} // namespace Renderer_System
