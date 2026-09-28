#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Debug_Utils.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Time of one scope of a frame, as read back by Gpu_Timer::Read_frame.
    struct Gpu_Scope_Timing
    {
        const char* name = nullptr;     // name given to Begin_scope
        const char* parent = nullptr;   // name of the enclosing scope; nullptr at the top level
        uint32_t    depth = 0;          // 0 = top level, 1 = inside a top-level scope, ...
        double      ms = 0.0;           // end point - begin point
    };

    // Everything measured in one frame.
    struct Gpu_Frame_Timings
    {
        // In recording order. Only the scopes the frame actually recorded.
        std::vector<Gpu_Scope_Timing> scopes;

        // Frame end point - frame start point.
        double total_ms = 0.0;

        // Sum of the top-level scopes. total_ms - top_level_ms is the time
        // spent outside every scope: barriers between scopes, copies that
        // belong to no scope, gaps.
        double top_level_ms = 0.0;

        // The frame was recorded in isolated mode (see Gpu_Timer): each
        // top-level scope measured alone, the total is not a frame time.
        bool   isolated = false;
    };

    // Gpu_Timer: GPU timestamps around named scopes of a frame, one range of
    // queries per frame in flight, read back without ever blocking.
    //
    // Recording, once per frame:
    //
    //   Begin_frame(cmd, slot, isolated);   // reset of the range + frame start point
    //   {
    //       Gpu_Scope scope(debug_utils, timer, cmd, "Light clusters", r, g, b);   // begin point
    //       ... commands of the scope ...
    //   }                                   // end point
    //   End_frame(cmd);                     // frame end point
    //
    // The results of a slot are read when that slot comes around again,
    // right after its fence was waited on: the submission that wrote them
    // has completed, so vkGetQueryPoolResults returns them without waiting.
    //
    // Every point is written at BOTTOM_OF_PIPE: a point is written once every
    // command submitted before it to the queue has completed, including the
    // command buffers of earlier frames. As a consequence:
    //   - a scope measures from "everything before it has finished" to "its
    //     own commands have finished";
    //   - work that overlaps is charged to whatever finishes later;
    //   - the frame total (end point - start point) is the GPU time of the
    //     frame counted from the moment the previous work completed, so two
    //     consecutive frames never count the same time twice, and the
    //     top-level scopes plus the time outside them add up to it.
    // A TOP_OF_PIPE point would be written as soon as the command processor
    // reaches it, and the specification allows writing it at any later
    // stage: the tail of the previous frame would be charged to whatever
    // follows it, by an amount that changes between GPUs and drivers.
    //
    // Scopes nest. The scope open when Begin_scope is called becomes the
    // parent of the new one, and scopes close in the reverse order they
    // were opened (Gpu_Scope guarantees it). Timestamps can be written
    // inside a render pass instance, so a scope may be opened inside one;
    // a top-level scope must be opened outside, because of the isolated
    // mode below.
    //
    // Isolated mode (Begin_frame with _isolated = true): before the begin
    // point of every top-level scope, a full pipeline barrier (all stages to
    // all stages, every memory write made available and visible) is
    // recorded. The scope then starts only when everything before it has
    // finished, and measures its own work without overlap. Nested scopes
    // are not isolated: the render pass declares no subpass
    // self-dependency, so no barrier can be recorded inside it. The mode
    // changes the performance of the frame it measures: its totals are not
    // frame times. It serves to compare one pass before and after a change.
    //
    // Each slot keeps what it recorded (name, begin point, end point and
    // parent of every scope) and how many points it wrote, and a read
    // queries exactly those points: a scope missing from a frame (a pass
    // that was not recorded) does not invalidate the frame.
    //
    // Capacity: _max_scopes_per_frame scopes per frame, two points each,
    // plus the frame start and end points. A scope beyond the capacity is
    // not timed (its label is still recorded) and reported once.
    //
    // A queue family without timestamp support (timestampValidBits 0)
    // leaves the timer inert: no query pool, every call a no-op, no
    // isolation barrier, and Read_frame always false.
    //
    // Known limitation: with FIFO presentation the submit waits for the
    // acquired image at COLOR_ATTACHMENT_OUTPUT. On drivers where
    // vkAcquireNextImageKHR returns an image before it is free, that wait
    // (the vertical sync) falls inside the scope that first writes the
    // swapchain image, the render pass. Measure without vsync, or take it
    // into account when reading the numbers.
    //
    // Not copyable or movable: owns the VkQueryPool.
    class Gpu_Timer
    {
    public:

        // Returned by Begin_scope when the scope is not timed (timestamps
        // unsupported, no frame being recorded, capacity exceeded).
        // End_scope accepts it and does nothing.
        static constexpr uint32_t INVALID_SCOPE = UINT32_MAX;

        // _frame_slots: frames in flight. _max_scopes_per_frame: scopes one
        // frame may time, at least 1.
        Gpu_Timer(const Vulkan_Device& _device, uint32_t _frame_slots, uint32_t _max_scopes_per_frame);
        ~Gpu_Timer();

        Gpu_Timer(const Gpu_Timer&) = delete;
        Gpu_Timer& operator=(const Gpu_Timer&) = delete;
        Gpu_Timer(Gpu_Timer&&) = delete;
        Gpu_Timer& operator=(Gpu_Timer&&) = delete;

        bool Is_supported() const { return query_pool != VK_NULL_HANDLE; }

        VkQueryPool Get_query_pool() const { return query_pool; }

        // Starts recording the frame of _frame_slot: forgets the scopes the
        // slot recorded before, resets its queries and writes the frame
        // start point. Recorded at the start of the frame's command buffer,
        // outside a render pass. _isolated selects the isolated mode for
        // this frame (see the class comment).
        void Begin_frame(VkCommandBuffer _command_buffer, uint32_t _frame_slot, bool _isolated);

        // Opens scope _name in the frame being recorded and returns its
        // index, or INVALID_SCOPE when it is not timed. In isolated mode a
        // top-level scope records its barrier first. _name must point to a
        // string with static storage duration (a literal): the timer and
        // the statistics keep the pointer across frames.
        uint32_t Begin_scope(VkCommandBuffer _command_buffer, const char* _name);

        // Closes _scope, which must be the innermost open scope, and writes
        // its end point.
        void End_scope(VkCommandBuffer _command_buffer, uint32_t _scope);

        // Writes the frame end point after every command recorded so far.
        // Every scope must be closed.
        void End_frame(VkCommandBuffer _command_buffer);

        // Reads the last frame recorded in _frame_slot into _out (its
        // vectors keep their capacity). Returns false, leaving _out
        // untouched, when timestamps are unsupported, the slot never
        // recorded a complete frame, or the results are not available.
        //
        // Precondition: the fence of the submission that recorded the slot
        // has been waited on, and the slot has not been recorded again yet.
        bool Read_frame(uint32_t _frame_slot, Gpu_Frame_Timings& _out);

    private:

        static constexpr uint32_t INVALID_SLOT = UINT32_MAX;

        struct Scope_Record
        {
            const char* name = nullptr;
            uint32_t    parent = INVALID_SCOPE;   // index of the enclosing scope
            uint32_t    depth = 0;
            uint32_t    begin_point = 0;          // relative to the first query of the slot
            uint32_t    end_point = 0;
        };

        struct Slot_State
        {
            std::vector<Scope_Record> scopes;               // recording order
            uint32_t                  points_written = 0;   // point 0 is the frame start
            uint32_t                  open_scope = INVALID_SCOPE;
            bool                      isolated = false;
            bool                      complete = false;     // End_frame was recorded
        };

        // Writes point _point of the recording slot at BOTTOM_OF_PIPE.
        void Write_point(VkCommandBuffer _command_buffer, uint32_t _point) const;

        // Full barrier recorded before a top-level scope in isolated mode.
        static void Record_isolation_barrier(VkCommandBuffer _command_buffer);

        VkDevice    device_handle;
        VkQueryPool query_pool;
        uint32_t    frame_slots;
        uint32_t    max_scopes;

        // 2 + 2 * max_scopes: frame start and end, begin and end per scope.
        uint32_t    points_per_frame;

        // (1 << timestampValidBits) - 1: the bits a timestamp actually
        // carries. Differences are taken modulo this mask, so a counter
        // that wraps between two points still yields the right interval.
        uint64_t    valid_mask;

        // Nanoseconds per tick (VkPhysicalDeviceLimits::timestampPeriod).
        double      period_ns;

        std::vector<Slot_State> slots;

        // Slot between Begin_frame and End_frame; INVALID_SLOT otherwise.
        uint32_t    recording_slot;

        // The capacity warning is printed once.
        bool        warned_capacity;

        // Scratch for vkGetQueryPoolResults, reused every read.
        std::vector<uint64_t> raw_results;
    };

    // Gpu_Scope: one named region of the command buffer, which is at the
    // same time a debug label (VK_EXT_debug_utils, shown by RenderDoc and
    // Nsight) and a timed scope of the Gpu_Timer, under the same name. Both
    // start in the constructor and end in the destructor, so the label a
    // capture shows and the scope the timer prints cannot diverge, and an
    // early return or an exception cannot leave either open.
    //
    // _name must be a string literal (see Gpu_Timer::Begin_scope).
    class Gpu_Scope
    {
    public:
        Gpu_Scope(const Vulkan_Debug_Utils& _debug_utils, Gpu_Timer& _timer, VkCommandBuffer _command_buffer,
                  const char* _name, float _red, float _green, float _blue)
            : debug_utils(_debug_utils), timer(_timer), command_buffer(_command_buffer)
        {
            // Label first: in isolated mode the barrier of the scope belongs
            // to the labeled region as well.
            debug_utils.Begin_label(command_buffer, _name, _red, _green, _blue);
            scope = timer.Begin_scope(command_buffer, _name);
        }

        ~Gpu_Scope()
        {
            timer.End_scope(command_buffer, scope);
            debug_utils.End_label(command_buffer);
        }

        Gpu_Scope(const Gpu_Scope&) = delete;
        Gpu_Scope& operator=(const Gpu_Scope&) = delete;

    private:
        const Vulkan_Debug_Utils& debug_utils;
        Gpu_Timer&                timer;
        VkCommandBuffer           command_buffer;
        uint32_t                  scope = Gpu_Timer::INVALID_SCOPE;
    };

} // namespace Renderer_System
