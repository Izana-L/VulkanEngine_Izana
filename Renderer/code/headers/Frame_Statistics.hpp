#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Gpu_Layouts.hpp>
#include <Gpu_Timer.hpp>
#include <Render_Debug_Settings.hpp>
#include <Vulkan_Buffer_Utils.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Frame_Statistics: what the Renderer measures about its frames, and
    // how it reports it.
    //
    // Two kinds of data reach it, both read after the last submission of
    // the frame slot that produced them completed (its serial was waited
    // on), so the CPU never stalls for them:
    //   - the GPU counters of the frame (Frame_Stats_GPU): cluster light
    //     references, dropped lights, objects the culling pass kept and
    //     objects it had to drop;
    //   - the timestamps of its named scopes (Gpu_Frame_Timings).
    // Each frame is paired with the Frame_Record of what the CPU recorded
    // for it.
    //
    // Statistics only: nothing the Renderer's behaviour depends on lives
    // here. State that means "this is already on the GPU" is applied by the
    // Renderer after the submit (Frame_Effects), not read back from a
    // statistics record.
    //
    // Timings and counters are averaged over a reporting interval (one
    // second) and printed when printing is enabled; the accumulators are
    // reset after every interval. The accounting receives plain values and
    // needs no device; the two static functions below are the only ones
    // that touch Vulkan, and own the readback protocol of the counters.
    class Frame_Statistics
    {
    public:

        // What a frame slot recorded, kept until its fence is waited on so
        // the GPU counters can be read with their context.
        struct Frame_Record
        {
            bool             recorded = false;
            Opaque_Draw_Path opaque_path = Opaque_Draw_Path::Direct;

            // Valid opaque items of the frame, before any frustum test.
            uint32_t         opaque_candidates = 0;

            // Opaque objects written to the object buffer: the candidates
            // that survived the CPU frustum test, when there is one.
            uint32_t         opaque_objects = 0;

            uint32_t         transparent_candidates = 0;
            uint32_t         transparent_drawn = 0;
        };

        // Fill of the Geometry_Pool, shown in the report.
        struct Geometry_Usage
        {
            uint32_t used_vertices = 0;
            uint32_t vertex_capacity = 0;
            uint32_t used_indices = 0;
            uint32_t index_capacity = 0;
        };

        // _slot_count: frame slots; _max_scopes: timed scopes one frame
        // may record (storage is reserved for them).
        Frame_Statistics(uint32_t _slot_count, uint32_t _max_scopes);

        // =====================================================
        // Counter readback
        // =====================================================

        // Records, after the compute -> consumers barrier of a frame, the
        // copies of its GPU counters into _readback (one Frame_Stats_GPU,
        // host-visible) and the barrier that makes them visible to the
        // host. Recorded outside a render pass. The buffers must have been
        // created with TRANSFER_SRC (the counters) and TRANSFER_DST (the
        // readback).
        //
        // Read when the frame slot comes around again (Read_counters), so
        // the CPU never waits for them.
        static void Record_readback(VkCommandBuffer _command_buffer,
                                    const Vulkan_Buffer_Utils::Buffer_Allocation& _cluster_counters,
                                    const Vulkan_Buffer_Utils::Buffer_Allocation& _draw_count,
                                    const Vulkan_Buffer_Utils::Buffer_Allocation& _readback);

        // Reads the counters copied by Record_readback. Precondition: the
        // frame that recorded the copies has completed (its serial was
        // waited on).
        static Frame_Stats_GPU Read_counters(VmaAllocator _allocator,
                                             const Vulkan_Buffer_Utils::Buffer_Allocation& _readback);

        // =====================================================
        // Frame records
        // =====================================================

        // Stores the context of the frame just recorded for _slot. The
        // frame is not pending until Mark_submitted: a recording that threw
        // or a failed submit leaves nothing to read back.
        void Stage_record(uint32_t _slot, const Frame_Record& _record);

        // The staged frame of _slot was submitted: its counters will be
        // read when the slot comes around again.
        void Mark_submitted(uint32_t _slot);

        // Hands out the record of the last frame submitted from _slot and
        // clears it. False when nothing was submitted from the slot since
        // its last read.
        bool Take_pending(uint32_t _slot, Frame_Record& _out_record);

        // =====================================================
        // Accumulation and report
        // =====================================================

        // Adds one measured frame: its GPU counters, and its timings when
        // available (_timings may be null). Reports once when the cluster
        // light index list overflowed, whether statistics are printed or
        // not: a full list silently removes lights from clusters.
        //
        // _isolated: the timing mode selected now. A frame measured in the
        // other mode (the switch changed while it was in flight) is not
        // mixed into the averages.
        void Accumulate(const Frame_Record& _record, const Frame_Stats_GPU& _counters,
                        const Gpu_Frame_Timings* _timings, bool _isolated);

        // Once per interval: prints the averages when _print_enabled, then
        // starts a new interval. _isolated is the timing mode, shown in the
        // report.
        void Report(bool _print_enabled, bool _isolated, const Geometry_Usage& _geometry);

        // Forgets everything accumulated and starts a new interval: the
        // next report covers only frames measured from now on.
        void Restart();

    private:

        // Accumulated time of one timed scope between two reports, keyed by
        // name and parent name.
        struct Scope_Statistics
        {
            const char* name = nullptr;
            const char* parent = nullptr;
            uint32_t    depth = 0;
            uint32_t    frames = 0;       // timed frames that recorded the scope
            double      ms_sum = 0.0;
        };

        // Adds the scopes of _timings to `scopes`, in recording order: a
        // scope first seen in a later frame is inserted after the scope that
        // preceded it in that frame.
        void Accumulate_scope_timings(const Gpu_Frame_Timings& _timings);

        // Prints the average of every accumulated scope, the time outside
        // the top-level scopes and the frame total.
        void Print_gpu_timings(bool _isolated) const;

        // Clears the accumulators, keeping the last counters and record.
        void Reset_accumulators();

        std::vector<Frame_Record>                 records;

        uint32_t                                  frames = 0;
        uint32_t                                  timed_frames = 0;
        double                                    total_ms_sum = 0.0;   // frame end - frame start
        double                                    other_ms_sum = 0.0;   // total - top-level scopes

        std::vector<Scope_Statistics>             scopes;

        Frame_Stats_GPU                           last_counters{};
        Frame_Record                              last_record{};
        std::chrono::steady_clock::time_point     last_print{};

        // The cluster light index list overflowed; reported once.
        bool                                      warned_cluster_overflow = false;

        // The culling pass dropped objects it had kept; reported once.
        bool                                      warned_dropped_draws = false;
    };

} // namespace Renderer_System
