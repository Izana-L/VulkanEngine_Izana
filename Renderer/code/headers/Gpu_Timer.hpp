#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Gpu_Timer: GPU timestamps around the passes of a frame, one range of
    // queries per frame in flight, read back without ever blocking.
    //
    // Every frame writes the same sequence of points into the range of its
    // frame slot:
    //
    //   Begin_frame(cmd, slot);        // reset of the range + point 0
    //   ... pass A ...
    //   Write(cmd, slot, 1);           // end of A
    //   ... pass B ...
    //   Write(cmd, slot, 2);           // end of B
    //
    // and the interval i is the time between point i and point i + 1. The
    // results of a slot are read when that slot comes around again, right
    // after its fence was waited on: the submission that wrote them has
    // completed, so vkGetQueryPoolResults returns them without waiting.
    //
    // Point 0 is written at TOP_OF_PIPE, the other points at
    // BOTTOM_OF_PIPE: a point is written when every command recorded
    // before it has completed, so the intervals of consecutive passes add
    // up to the frame time on the GPU and overlapping work is attributed
    // to the pass that finishes last.
    //
    // Every point of a frame must be written in every frame: one missing
    // point leaves the range unavailable and the whole frame is not
    // reported.
    //
    // A queue family without timestamp support (timestampValidBits 0)
    // leaves the timer inert: no query pool, every call a no-op, and
    // Read_intervals always false.
    //
    // Not copyable or movable: owns the VkQueryPool.
    class Gpu_Timer
    {
    public:

        // _frame_slots: frames in flight. _points_per_frame: timestamps
        // written per frame, at least 2 (one interval).
        Gpu_Timer(const Vulkan_Device& _device, uint32_t _frame_slots, uint32_t _points_per_frame);
        ~Gpu_Timer();

        Gpu_Timer(const Gpu_Timer&) = delete;
        Gpu_Timer& operator=(const Gpu_Timer&) = delete;
        Gpu_Timer(Gpu_Timer&&) = delete;
        Gpu_Timer& operator=(Gpu_Timer&&) = delete;

        bool Is_supported() const { return query_pool != VK_NULL_HANDLE; }

        VkQueryPool Get_query_pool() const { return query_pool; }

        // Resets the queries of _frame_slot and writes point 0. Recorded at
        // the start of the frame's command buffer, outside a render pass.
        void Begin_frame(VkCommandBuffer _command_buffer, uint32_t _frame_slot);

        // Writes point _point (1 .. points_per_frame - 1) of _frame_slot,
        // after every command recorded before it completes.
        void Write(VkCommandBuffer _command_buffer, uint32_t _frame_slot, uint32_t _point) const;

        // Reads the last frame recorded in _frame_slot and stores its
        // points_per_frame - 1 intervals, in milliseconds, into
        // _out_intervals_ms. Returns false, leaving the output untouched,
        // when timestamps are unsupported, the slot was never recorded, or
        // the results are not available.
        //
        // Precondition: the fence of the submission that recorded the slot
        // has been waited on, and the slot has not been recorded again yet.
        bool Read_intervals(uint32_t _frame_slot, std::vector<double>& _out_intervals_ms);

    private:

        VkDevice    device_handle;
        VkQueryPool query_pool;
        uint32_t    frame_slots;
        uint32_t    points_per_frame;

        // (1 << timestampValidBits) - 1: the bits a timestamp actually
        // carries. Differences are taken modulo this mask, so a counter
        // that wraps between two points still yields the right interval.
        uint64_t    valid_mask;

        // Nanoseconds per tick (VkPhysicalDeviceLimits::timestampPeriod).
        double      period_ns;

        // Per slot: whether Begin_frame was recorded since creation, i.e.
        // whether the range holds a frame worth reading.
        std::vector<bool> recorded;

        // Scratch for vkGetQueryPoolResults, reused every read.
        std::vector<uint64_t> raw_results;
    };

} // namespace Renderer_System
