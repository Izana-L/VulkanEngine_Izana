#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Frame_Timeline: the serial numbers of the frames submitted to the GPU
    // and which of them are known to have completed.
    //
    // Every submitted frame gets the next serial (1, 2, 3...; 0 means "no
    // frame yet"). A resource that the GPU may still read is tagged with
    // the serial of the last frame that can read it, and may be reused or
    // destroyed once Is_complete(serial) holds.
    //
    // The serials are the values of the timeline semaphore that every
    // submission of a frame signals (Renderer::Impl::frame_semaphore): the
    // semaphore reaches serial N when the submission N has completed, and a
    // timeline semaphore is monotonic, so it has then also reached every
    // earlier serial. Completion is learned from its counter: the owner
    // waits on the semaphore or reads its value, and reports the result
    // here. Nothing here talks to the device.
    //
    // A serial is consumed only by a submission that succeeded
    // (Record_submission). The next submission after a failed one signals
    // the same serial again, so the counter never skips a value and never
    // waits for one that no submission will signal.
    class Frame_Timeline
    {
    public:

        // _slot_count: number of frame slots (frames in flight).
        explicit Frame_Timeline(uint32_t _slot_count)
            : slot_serials(_slot_count, 0)
        {
            assert(_slot_count > 0 && "Frame_Timeline needs at least one slot");
        }

        // Registers a submission made from _slot and returns its serial.
        // Called only after the submission succeeded: a frame that was
        // never submitted has no serial and nothing waits for it.
        uint64_t Record_submission(uint32_t _slot)
        {
            assert(_slot < slot_serials.size() && "Frame_Timeline: slot out of range");

            slot_serials[_slot] = ++submitted;
            return slot_serials[_slot];
        }

        // The counter of the timeline semaphore was observed at _value (by
        // a wait that returned, or by reading it): every serial up to
        // _value has completed. A value beyond the last submitted serial is
        // clamped, so a stale reading cannot mark a frame that was never
        // submitted.
        void Mark_completed(uint64_t _value)
        {
            completed = std::max(completed, std::min(_value, submitted));
        }

        // Every submitted frame has completed: the semaphore was waited on
        // up to the last serial, or the device went idle.
        void Mark_all_complete()
        {
            completed = submitted;
        }

        // True when the frame with _serial, and every earlier one, has
        // completed. Serial 0 (nothing submitted) is always complete.
        bool Is_complete(uint64_t _serial) const
        {
            return _serial <= completed;
        }

        // Serial of the last frame submitted; the tag of a resource that
        // every frame submitted so far may still read.
        uint64_t Get_submitted_serial() const { return submitted; }

        // Highest serial known to be complete.
        uint64_t Get_completed_serial() const { return completed; }

        // Serial last submitted from _slot (0 if the slot never submitted).
        uint64_t Get_slot_serial(uint32_t _slot) const
        {
            assert(_slot < slot_serials.size() && "Frame_Timeline: slot out of range");

            return slot_serials[_slot];
        }

        uint32_t Get_slot_count() const { return static_cast<uint32_t>(slot_serials.size()); }

    private:

        uint64_t              submitted = 0;
        uint64_t              completed = 0;
        std::vector<uint64_t> slot_serials;
    };

} // namespace Renderer_System
