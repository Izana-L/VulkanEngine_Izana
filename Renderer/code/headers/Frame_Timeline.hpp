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
    // Completion is learned from fences. The fence of a frame slot signals
    // after every submission made before it, so waiting on the fence of a
    // slot completes the serial that slot submitted last and, implicitly,
    // every earlier serial. Nothing here talks to the device: the owner
    // waits on the fences and reports the result.
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

        // The fence of _slot was waited on: the last serial submitted from
        // it, and every serial before it, have completed.
        void Mark_slot_complete(uint32_t _slot)
        {
            assert(_slot < slot_serials.size() && "Frame_Timeline: slot out of range");

            completed = std::max(completed, slot_serials[_slot]);
        }

        // Every submitted frame has completed: the fences of all slots were
        // waited on, or the device went idle.
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
