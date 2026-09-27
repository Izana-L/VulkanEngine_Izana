#include <Gpu_Timer.hpp>
#include <Vulkan_Utils.hpp>

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace Renderer_System
{

    Gpu_Timer::Gpu_Timer(const Vulkan_Device& _device, uint32_t _frame_slots, uint32_t _points_per_frame)
        : device_handle(_device.Get_logical_device_handle()),
        query_pool(VK_NULL_HANDLE),
        frame_slots(_frame_slots),
        points_per_frame(_points_per_frame),
        valid_mask(0),
        period_ns(static_cast<double>(_device.Get_timestamp_period())),
        recorded(_frame_slots, false),
        raw_results(_points_per_frame, 0)
    {
        if (_frame_slots == 0 || _points_per_frame < 2)
            throw std::invalid_argument("Gpu_Timer: at least one frame slot and two points per frame are required");

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

    void Gpu_Timer::Begin_frame(VkCommandBuffer _command_buffer, uint32_t _frame_slot)
    {
        assert(_frame_slot < frame_slots && "Gpu_Timer::Begin_frame: frame slot out of range");

        if (query_pool == VK_NULL_HANDLE)
            return;

        const uint32_t first_query = _frame_slot * points_per_frame;

        // A query must be reset before it is written again; resetting the
        // range here, in the same command buffer, needs no host reset
        // feature and cannot race the read of the previous frame, which
        // happened before this command buffer was recorded.
        vkCmdResetQueryPool(_command_buffer, query_pool, first_query, points_per_frame);
        vkCmdWriteTimestamp(_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, first_query);

        recorded[_frame_slot] = true;
    }

    void Gpu_Timer::Write(VkCommandBuffer _command_buffer, uint32_t _frame_slot, uint32_t _point) const
    {
        assert(_frame_slot < frame_slots && "Gpu_Timer::Write: frame slot out of range");
        assert(_point > 0 && _point < points_per_frame && "Gpu_Timer::Write: point out of range (point 0 is Begin_frame's)");

        if (query_pool == VK_NULL_HANDLE)
            return;

        vkCmdWriteTimestamp(_command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool,
            _frame_slot * points_per_frame + _point);
    }

    bool Gpu_Timer::Read_intervals(uint32_t _frame_slot, std::vector<double>& _out_intervals_ms)
    {
        assert(_frame_slot < frame_slots && "Gpu_Timer::Read_intervals: frame slot out of range");

        if (query_pool == VK_NULL_HANDLE || !recorded[_frame_slot])
            return false;

        // No WAIT flag: the fence of the slot was waited on, so the results
        // are available. VK_NOT_READY means a point was never written (the
        // command buffer was not submitted, or a point is missing).
        const VkResult result = vkGetQueryPoolResults(device_handle, query_pool,
            _frame_slot * points_per_frame, points_per_frame,
            raw_results.size() * sizeof(uint64_t), raw_results.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);

        if (result != VK_SUCCESS)
            return false;

        _out_intervals_ms.resize(points_per_frame - 1);

        for (uint32_t i = 0; i + 1 < points_per_frame; ++i)
        {
            const uint64_t ticks = (raw_results[i + 1] - raw_results[i]) & valid_mask;
            _out_intervals_ms[i] = static_cast<double>(ticks) * period_ns / 1'000'000.0;
        }

        return true;
    }

} // namespace Renderer_System
