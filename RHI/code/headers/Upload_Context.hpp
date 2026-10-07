#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Command_Pool.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Handles.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace Renderer_System
{

    // Upload_Context: the machinery of synchronous transfers to the GPU
    // (asset uploads and one-off initialization work).
    //
    // One transfer is a command buffer from a dedicated transient pool,
    // recorded by the caller, submitted to the graphics queue and waited
    // for on the CPU. Run does the whole sequence, undoing it on failure:
    //
    //   context.Run([&](VkCommandBuffer _cmd)
    //   {
    //       ... record, staging buffers from Create_staging() ...
    //   });
    //
    // which is Begin, the recording, Submit_and_wait and End, with Abort on
    // any failure in between (the steps are public for a caller that needs
    // to put its own work between them).
    //
    // Staging buffers created through the context stay alive until End() or
    // Abort(), so their contents outlive the GPU copy. Whoever records into
    // the command buffer (Texture_GPU, the mesh copies of the Renderer)
    // asks the context for its staging memory, so there is one owner of the
    // staging buffers of a transfer and one place that frees them.
    //
    // The pool is separate from the per-frame pools, so a transfer never
    // interferes with a frame in flight. The wait uses a fence, not
    // vkQueueWaitIdle, which would also stall every frame already
    // submitted to the graphics queue.
    //
    // One transfer at a time. Not copyable or movable.
    class Upload_Context
    {
    public:

        Upload_Context(const Vulkan_Device& _device, VmaAllocator _allocator);
        ~Upload_Context();

        Upload_Context(const Upload_Context&) = delete;
        Upload_Context& operator=(const Upload_Context&) = delete;
        Upload_Context(Upload_Context&&) = delete;
        Upload_Context& operator=(Upload_Context&&) = delete;

        // One whole transfer: Begin, _record(command buffer), Submit_and_
        // wait and End. _record writes the commands and asks Create_staging
        // for the memory they read.
        //
        // If anything throws, from the begin to the wait, the transfer is
        // undone (Abort) before the exception propagates: when it returns
        // normally the GPU has executed the commands and consumed every
        // staging buffer, and when it throws nothing recorded can still be
        // running, with one exception: when Is_usable() is false after the
        // throw, the wait for the device failed too (see Abort), the GPU may
        // still be executing the commands, and whatever they write must not
        // be released by the caller's handler.
        //
        // What _record created for itself (images, registry entries) is the
        // caller's to undo, in its own handler, which runs after the device
        // went idle (unless Is_usable() says otherwise, as above). If the
        // device is lost while the transfer is undone, the exception is a
        // Vulkan_Utils::Vulkan_Error with VK_ERROR_DEVICE_LOST instead of
        // the original one, whose message it carries: a lost device
        // outranks whatever failed first.
        template <typename Record>
        void Run(Record&& _record)
        {
            // Begin throws with nothing left allocated: nothing to undo.
            const VkCommandBuffer command_buffer = Begin();

            try
            {
                std::forward<Record>(_record)(command_buffer);
                Submit_and_wait(command_buffer);
            }
            catch (...)
            {
                Abort_after_failure(command_buffer);   // may replace the exception
                throw;
            }

            End(command_buffer);
        }

        // Allocates a primary command buffer and begins it for a single
        // submission. Throws with nothing left allocated, and
        // std::runtime_error if an earlier Abort could not wait for the
        // device (see Abort): the context is not usable then.
        VkCommandBuffer Begin();

        // Creates a host-visible, persistently mapped TRANSFER_SRC buffer of
        // _size bytes, released by End() or Abort(). Writes through
        // mapped_ptr need no flush before the submit.
        Vulkan_Buffer_Utils::Buffer_Allocation Create_staging(VkDeviceSize _size);

        // Ends _command_buffer, submits it to the graphics queue signaling
        // the context fence, and blocks until the fence signals.
        void Submit_and_wait(VkCommandBuffer _command_buffer);

        // Success path, after Submit_and_wait: the GPU has consumed every
        // staging buffer, which are destroyed together with the command
        // buffer.
        void End(VkCommandBuffer _command_buffer);

        // Failure path, at any point after Begin: waits for the device to
        // go idle, so nothing recorded can still be executing, then
        // destroys the staging buffers and the command buffer. The caller
        // undoes its own registrations after this returns. Does not throw.
        //
        // Returns the result of the wait. VK_SUCCESS: everything was
        // released. VK_ERROR_DEVICE_LOST: the device executes nothing any
        // more, so everything was released too, and the caller must treat
        // the device as lost. Any other result (out of memory while
        // waiting) means the GPU may still be reading the staging buffers:
        // they and the command buffer are left allocated, released with the
        // context, and Begin refuses further transfers.
        [[nodiscard]] VkResult Abort(VkCommandBuffer _command_buffer) noexcept;

        // False once an Abort could not wait for the device (see Abort): a
        // transfer may still be executing, so nothing it wrote may be
        // released and Begin refuses further transfers. True otherwise.
        [[nodiscard]] bool Is_usable() const noexcept { return !unusable; }

    private:

        // The failure path of Run: Abort, and Vulkan_Error with
        // VK_ERROR_DEVICE_LOST when the wait reports a lost device. Called
        // from a catch block, which rethrows the original exception when
        // this returns.
        void Abort_after_failure(VkCommandBuffer _command_buffer);

        const Vulkan_Device&                                 device;
        VmaAllocator                                         allocator;

        Vulkan_Command_Pool                                  command_pool;

        // Shared by every transfer and reset before each submit, instead of
        // created and destroyed per transfer. Not created signaled: its
        // state at creation is irrelevant. Owned by a wrapper, so it is
        // released even when the constructor fails after creating it.
        Unique_Fence                                         fence;

        std::vector<Vulkan_Buffer_Utils::Buffer_Allocation>  staging_buffers;

        // An Abort could not wait for the device to go idle (and the device
        // is not lost): the resources of that transfer are still allocated
        // and no further transfer may start.
        bool                                                 unusable = false;

        void Destroy_staging_buffers() noexcept;
    };

} // namespace Renderer_System
