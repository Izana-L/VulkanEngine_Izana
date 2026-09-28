#include <Frame_Statistics.hpp>
#include <Renderer_Limits.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <iostream>

namespace Renderer_System
{

    namespace
    {
        // Time between two reports.
        constexpr std::chrono::milliseconds STATISTICS_PRINT_INTERVAL{ 1000 };

        // Scope names are string literals, but the same literal may live at
        // different addresses: compared by content. nullptr (no parent)
        // only equals nullptr.
        bool Same_scope_name(const char* _a, const char* _b)
        {
            if (_a == _b)
                return true;

            if (_a == nullptr || _b == nullptr)
                return false;

            return std::strcmp(_a, _b) == 0;
        }
    }

    Frame_Statistics::Frame_Statistics(uint32_t _slot_count, uint32_t _max_scopes)
        : records(_slot_count),
        last_print(std::chrono::steady_clock::now())
    {
        scopes.reserve(_max_scopes);
    }

    void Frame_Statistics::Record_readback(VkCommandBuffer _command_buffer,
                                           const Vulkan_Buffer_Utils::Buffer_Allocation& _cluster_counters,
                                           const Vulkan_Buffer_Utils::Buffer_Allocation& _draw_count,
                                           const Vulkan_Buffer_Utils::Buffer_Allocation& _readback)
    {
        const VkBufferCopy cluster_copy{ 0, offsetof(Frame_Stats_GPU, cluster_light_references),
                                         sizeof(uint32_t) * 2 };   // light_index_count, dropped_light_count
        const VkBufferCopy draw_copy{ 0, offsetof(Frame_Stats_GPU, gpu_opaque_draws), sizeof(uint32_t) };

        vkCmdCopyBuffer(_command_buffer, _cluster_counters.buffer, _readback.buffer, 1, &cluster_copy);
        vkCmdCopyBuffer(_command_buffer, _draw_count.buffer, _readback.buffer, 1, &draw_copy);

        // Device writes reach host reads only through a barrier with the
        // host as destination; the fence wait then orders the read.
        Vulkan_Buffer_Utils::Record_memory_barrier(_command_buffer,
            { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT },
            { VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT });
    }

    Frame_Stats_GPU Frame_Statistics::Read_counters(VmaAllocator _allocator,
                                                    const Vulkan_Buffer_Utils::Buffer_Allocation& _readback)
    {
        Frame_Stats_GPU counters{};
        Vulkan_Buffer_Utils::Read_from_buffer(_allocator, _readback, &counters, sizeof(counters));
        return counters;
    }

    void Frame_Statistics::Stage_record(uint32_t _slot, const Frame_Record& _record)
    {
        assert(_slot < records.size() && "Frame_Statistics: slot out of range");

        records[_slot] = _record;
        records[_slot].recorded = false;
    }

    void Frame_Statistics::Mark_submitted(uint32_t _slot)
    {
        assert(_slot < records.size() && "Frame_Statistics: slot out of range");

        records[_slot].recorded = true;
    }

    bool Frame_Statistics::Recorded_procedural(uint32_t _slot) const
    {
        assert(_slot < records.size() && "Frame_Statistics: slot out of range");

        return records[_slot].procedural_recorded;
    }

    bool Frame_Statistics::Take_pending(uint32_t _slot, Frame_Record& _out_record)
    {
        assert(_slot < records.size() && "Frame_Statistics: slot out of range");

        Frame_Record& record = records[_slot];

        // Nothing submitted from this slot since its last read.
        if (!record.recorded)
            return false;

        record.recorded = false;
        _out_record = record;

        return true;
    }

    void Frame_Statistics::Accumulate(const Frame_Record& _record, const Frame_Stats_GPU& _counters,
                                      const Gpu_Frame_Timings* _timings, bool _isolated)
    {
        // Reported whether statistics are printed or not: a full list
        // silently removes lights from clusters.
        if (_counters.cluster_lights_dropped > 0 && !warned_cluster_overflow)
        {
            std::cerr << "[Renderer] The cluster light index list overflowed: " << _counters.cluster_light_references
                << " entries requested, " << CLUSTER_LIGHT_INDEX_CAPACITY << " available; " << _counters.cluster_lights_dropped
                << " light references dropped (magenta in the heatmap). Raise CLUSTER_AVERAGE_LIGHTS or lower the light ranges. "
                "Further occurrences are not reported.\n";
            warned_cluster_overflow = true;
        }

        ++frames;

        // A frame measured in the other timing mode (the switch changed
        // while it was in flight) is not mixed into the averages.
        if (_timings != nullptr && _timings->isolated == _isolated)
        {
            ++timed_frames;
            total_ms_sum += _timings->total_ms;
            other_ms_sum += _timings->total_ms - _timings->top_level_ms;

            Accumulate_scope_timings(*_timings);
        }

        last_counters = _counters;
        last_record = _record;
    }

    void Frame_Statistics::Report(bool _print_enabled, bool _isolated, const Geometry_Usage& _geometry)
    {
        const auto now = std::chrono::steady_clock::now();

        if (now - last_print < STATISTICS_PRINT_INTERVAL)
            return;

        if (_print_enabled)
        {
            const std::ios_base::fmtflags previous_flags = std::cout.flags();
            const std::streamsize         previous_precision = std::cout.precision();

            std::cout << std::fixed << std::setprecision(3);

            Print_gpu_timings(_isolated);

            const Frame_Record& last = last_record;

            // The GPU paths report what the culling pass kept; the CPU paths
            // draw every object they wrote.
            const uint32_t opaque_drawn = Is_gpu_draw_path(last.opaque_path) ? last_counters.gpu_opaque_draws : last.opaque_objects;

            std::cout << "[Renderer] Opaque drawn " << opaque_drawn << " / " << last.opaque_objects << " (" << To_string(last.opaque_path)
                << ") | transparent drawn " << last.transparent_drawn << " / " << last.transparent_candidates
                << " | cluster light references " << last_counters.cluster_light_references << " / " << CLUSTER_LIGHT_INDEX_CAPACITY
                << " (dropped " << last_counters.cluster_lights_dropped << ") | geometry pool "
                << _geometry.used_vertices << " / " << _geometry.vertex_capacity << " vertices, "
                << _geometry.used_indices << " / " << _geometry.index_capacity << " indices\n";

            std::cout.flags(previous_flags);
            std::cout.precision(previous_precision);
        }

        Reset_accumulators();
        last_print = now;
    }

    void Frame_Statistics::Restart()
    {
        Reset_accumulators();
        last_print = std::chrono::steady_clock::now();
    }

    void Frame_Statistics::Reset_accumulators()
    {
        frames = 0;
        timed_frames = 0;
        total_ms_sum = 0.0;
        other_ms_sum = 0.0;
        scopes.clear();
    }

    void Frame_Statistics::Accumulate_scope_timings(const Gpu_Frame_Timings& _timings)
    {
        // Merged in recording order: a scope already known keeps its place,
        // a new one goes right after the scope that preceded it in this
        // frame, so the print follows the order of the frame even when a
        // scope (procedural, culling) is missing from the first frames.
        size_t insert_at = 0;

        for (const Gpu_Scope_Timing& timing : _timings.scopes)
        {
            auto it = std::find_if(scopes.begin(), scopes.end(), [&](const Scope_Statistics& _scope)
                {
                    return Same_scope_name(_scope.name, timing.name) && Same_scope_name(_scope.parent, timing.parent);
                });

            if (it == scopes.end())
            {
                Scope_Statistics scope;
                scope.name = timing.name;
                scope.parent = timing.parent;
                scope.depth = timing.depth;

                it = scopes.insert(scopes.begin() + static_cast<std::ptrdiff_t>(std::min(insert_at, scopes.size())), scope);
            }

            ++it->frames;
            it->ms_sum += timing.ms;

            insert_at = static_cast<size_t>(it - scopes.begin()) + 1;
        }
    }

    void Frame_Statistics::Print_gpu_timings(bool _isolated) const
    {
        if (timed_frames == 0)
        {
            std::cout << "[Renderer] GPU timings unavailable (no timestamp support, or no frame measured yet).\n";
            return;
        }

        const double measured_frames = static_cast<double>(timed_frames);

        // Every average is taken over all the measured frames, a scope
        // missing from a frame counting 0 there, so the top-level scopes
        // plus "other" add up to the total. A scope recorded in only some
        // of the frames also shows its cost in those frames.
        const auto print_scope = [&](const Scope_Statistics& _scope)
            {
                std::cout << (_scope.name ? _scope.name : "?") << " " << (_scope.ms_sum / measured_frames);

                if (_scope.frames < timed_frames && _scope.frames > 0)
                {
                    std::cout << " (in " << _scope.frames << " of " << timed_frames << " frames, "
                        << (_scope.ms_sum / static_cast<double>(_scope.frames)) << " each)";
                }
            };

        // Nested scopes follow their parent in brackets, at any depth.
        const auto print_children = [&](const auto& _self, const Scope_Statistics& _parent) -> void
            {
                bool first = true;

                for (const Scope_Statistics& child : scopes)
                {
                    if (child.depth != _parent.depth + 1 || !Same_scope_name(child.parent, _parent.name))
                        continue;

                    std::cout << (first ? " [" : ", ");
                    print_scope(child);
                    _self(_self, child);
                    first = false;
                }

                if (!first)
                    std::cout << "]";
            };

        std::cout << "[Renderer] GPU ms";

        if (_isolated)
            std::cout << ", ISOLATED scopes (a full barrier before each: every scope measures its pass alone, the total is not a frame time)";

        std::cout << ", average of " << timed_frames << " frames: ";

        for (const Scope_Statistics& scope : scopes)
        {
            if (scope.depth != 0)
                continue;

            print_scope(scope);
            print_children(print_children, scope);
            std::cout << " | ";
        }

        // Barriers between scopes, the statistics copies and gaps.
        std::cout << "other " << (other_ms_sum / measured_frames) << " | total " << (total_ms_sum / measured_frames) << "\n";
    }

} // namespace Renderer_System
