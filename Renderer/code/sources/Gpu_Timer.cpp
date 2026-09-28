#include <Gpu_Timer.hpp>
#include <Vulkan_Utils.hpp>

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace Renderer_System
{

    Gpu_Timer::Gpu_Timer(const Vulkan_Device& _device, uint32_t _frame_slots, uint32_t _max_scopes_per_frame)
        : device_handle(_device.Get_logical_device_handle()),
        query_pool(VK_NULL_HANDLE),
        frame_slots(_frame_slots),
        max_scopes(_max_scopes_per_frame),
        points_per_frame(2 + 2 * _max_scopes_per_frame),
        valid_mask(0),
        period_ns(static_cast<double>(_device.Get_timestamp_period())),
        slots(_frame_slots),
        recording_slot(INVALID_SLOT),
        warned_capacity(false),
        raw_results(2 + 2 * static_cast<size_t>(_max_scopes_per_frame), 0)
    {
        if (_frame_slots == 0 || _max_scopes_per_frame == 0)
            throw std::invalid_argument("Gpu_Timer: at least one frame slot and one scope per frame are required");

        // Reserved once: recording a frame never allocates.
        for (Slot_State& slot : slots)
            slot.scopes.reserve(max_scopes);

        const uint32_t valid_bits = _device.Get_timestamp_valid_bits();

        // No timestamp support on the queue family that records the frame:
        // the timer stays inert instead of failing the Renderer.
        if (valid_bits == 0 || period_ns <= 0.0)
            return;

        valid_mask = (valid_bits >= 64) ? ~0ull : ((1ull << valid_bits) - 1ull);

        VkQueryPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        pool_info.queryCount = frame_slots * points_per_frame;

        VK_CHECK(vkCreateQueryPool(device_handle, &pool_info, nullptr, &query_pool),
            "Gpu_Timer: failed to create the timestamp query pool");
    }

    Gpu_Timer::~Gpu_Timer()
    {
        if (query_pool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(device_handle, query_pool, nullptr);
            query_pool = VK_NULL_HANDLE;
        }
    }

    void Gpu_Timer::Begin_frame(VkCommandBuffer _command_buffer, uint32_t _frame_slot, bool _isolated)
    {
        assert(_frame_slot < frame_slots && "Gpu_Timer::Begin_frame: frame slot out of range");

        // A frame whose recording was abandoned (an exception between
        // Begin_frame and End_frame) leaves recording_slot set; the new
        // frame simply takes over.
        recording_slot = _frame_slot;

        Slot_State& slot = slots[_frame_slot];
        slot.scopes.clear();
        slot.points_written = 0;
        slot.open_scope = INVALID_SCOPE;
        slot.isolated = _isolated;
        slot.complete = false;

        if (query_pool == VK_NULL_HANDLE)
            return;

        // A query must be reset before it is written again; resetting the
        // range here, in the same command buffer, needs no host reset
        // feature and cannot race the read of the previous frame, which
        // happened before this command buffer was recorded.
        vkCmdResetQueryPool(_command_buffer, query_pool, _frame_slot * points_per_frame, points_per_frame);

        // Frame start: written once the work submitted before this command
        // buffer (the previous frames) has completed.
        Write_point(_command_buffer, slot.points_written++);
    }

    uint32_t Gpu_Timer::Begin_scope(VkCommandBuffer _command_buffer, const char* _name)
    {
        if (query_pool == VK_NULL_HANDLE || recording_slot == INVALID_SLOT)
            return INVALID_SCOPE;

        Slot_State& slot = slots[recording_slot];

        // Each scope takes two points; the frame end point is reserved
        // apart, so capacity is only a matter of the scope count.
        if (slot.scopes.size() >= max_scopes)
        {
            if (!warned_capacity)
            {
                std::cerr << "[Gpu_Timer] More than " << max_scopes << " scopes in one frame: '" << (_name ? _name : "")
                    << "' and every further scope of a full frame are not timed. Raise the scope capacity of the timer. "
                    "Further occurrences are not reported.\n";
                warned_capacity = true;
            }
            return INVALID_SCOPE;
        }

        Scope_Record record;
        record.name = _name;
        record.parent = slot.open_scope;
        record.depth = (slot.open_scope == INVALID_SCOPE) ? 0 : slot.scopes[slot.open_scope].depth + 1;

        // Isolated mode: the scope starts once everything before it,
        // earlier frames included, has finished and its writes are
        // visible. Top-level scopes only: they are opened outside the
        // render pass, where a barrier needs no subpass self-dependency.
        if (record.depth == 0 && slot.isolated)
            Record_isolation_barrier(_command_buffer);

        record.begin_point = slot.points_written;
        Write_point(_command_buffer, slot.points_written++);

        const uint32_t index = static_cast<uint32_t>(slot.scopes.size());
        slot.scopes.push_back(record);
        slot.open_scope = index;

        return index;
    }

    void Gpu_Timer::End_scope(VkCommandBuffer _command_buffer, uint32_t _scope)
    {
        if (_scope == INVALID_SCOPE || query_pool == VK_NULL_HANDLE || recording_slot == INVALID_SLOT)
            return;

        Slot_State& slot = slots[recording_slot];

        assert(_scope < slot.scopes.size() && "Gpu_Timer::End_scope: scope index out of range");
        assert(_scope == slot.open_scope && "Gpu_Timer::End_scope: scopes must be closed in the reverse order they were opened");

        Scope_Record& record = slot.scopes[_scope];
        record.end_point = slot.points_written;
        Write_point(_command_buffer, slot.points_written++);

        slot.open_scope = record.parent;
    }

    void Gpu_Timer::End_frame(VkCommandBuffer _command_buffer)
    {
        if (recording_slot == INVALID_SLOT)
            return;

        Slot_State& slot = slots[recording_slot];

        assert(slot.open_scope == INVALID_SCOPE && "Gpu_Timer::End_frame: a scope is still open");

        if (query_pool != VK_NULL_HANDLE)
        {
            // Frame end: after every command of the frame.
            Write_point(_command_buffer, slot.points_written++);
            slot.complete = true;
        }

        recording_slot = INVALID_SLOT;
    }

    bool Gpu_Timer::Read_frame(uint32_t _frame_slot, Gpu_Frame_Timings& _out)
    {
        assert(_frame_slot < frame_slots && "Gpu_Timer::Read_frame: frame slot out of range");

        const Slot_State& slot = slots[_frame_slot];

        if (query_pool == VK_NULL_HANDLE || !slot.complete || slot.points_written < 2)
            return false;

        // Only the points the frame wrote: the rest of the range was reset
        // and never written, and including it would report VK_NOT_READY.
        // No WAIT flag: the fence of the slot was waited on, so the results
        // are available; VK_NOT_READY means the command buffer was not
        // submitted.
        const VkResult result = vkGetQueryPoolResults(device_handle, query_pool,
            _frame_slot * points_per_frame, slot.points_written,
            static_cast<size_t>(slot.points_written) * sizeof(uint64_t), raw_results.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);

        if (result != VK_SUCCESS)
            return false;

        const auto interval_ms = [&](uint32_t _begin, uint32_t _end)
            {
                const uint64_t ticks = (raw_results[_end] - raw_results[_begin]) & valid_mask;
                return static_cast<double>(ticks) * period_ns / 1'000'000.0;
            };

        _out.scopes.clear();
        _out.total_ms = interval_ms(0, slot.points_written - 1);
        _out.top_level_ms = 0.0;
        _out.isolated = slot.isolated;

        for (const Scope_Record& record : slot.scopes)
        {
            Gpu_Scope_Timing timing;
            timing.name = record.name;
            timing.parent = (record.parent == INVALID_SCOPE) ? nullptr : slot.scopes[record.parent].name;
            timing.depth = record.depth;
            timing.ms = interval_ms(record.begin_point, record.end_point);

            if (record.depth == 0)
                _out.top_level_ms += timing.ms;

            _out.scopes.push_back(timing);
        }

        return true;
    }

    void Gpu_Timer::Write_point(VkCommandBuffer _command_buffer, uint32_t _point) const
    {
        assert(recording_slot != INVALID_SLOT && _point < points_per_frame && "Gpu_Timer: point out of range");

        vkCmdWriteTimestamp(_command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool,
            recording_slot * points_per_frame + _point);
    }

    void Gpu_Timer::Record_isolation_barrier(VkCommandBuffer _command_buffer)
    {
        // Every stage of every earlier command finishes, and every write is
        // made available and visible, before any later command starts.
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        vkCmdPipelineBarrier(_command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            1, &barrier, 0, nullptr, 0, nullptr);
    }

} // namespace Renderer_System
