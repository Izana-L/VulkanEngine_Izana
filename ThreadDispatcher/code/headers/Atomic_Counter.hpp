#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ThreadDispatcher
{

    // Atomic_Counter: thread-safe completion counter used to track a group
    // of tasks. Every task decrements the counter when it finishes; threads
    // that need the group's results wait for the counter to reach zero.
    //
    // The counter's invariants are enforced in every build configuration,
    // not only under assert(): a counter that silently wrapped around
    // below zero would block every waiter forever, which is far harder to
    // diagnose than an exception thrown at the offending call site.
    //
    // What "done" means. The counter is DONE when every task has decremented
    // it AND every Decrement() call has finished running its callbacks
    // (on_decrement / on_zero). A waiter released by Wait() / Is_done() can
    // therefore destroy whatever those callbacks reference, and everything
    // the tasks and the callbacks wrote is visible to it.
    //
    // Failures. A group also carries the first exception raised by any of
    // its tasks (or by one of its callbacks), see Report_failure(). The
    // waiting primitives never throw; Rethrow_if_failed() hands the failure
    // to the thread that is waiting, whichever thread actually ran the task.
    class Atomic_Counter
    {

    private:

        // One 64-bit word holds everything a lock-free reader needs, so that
        // "pending tasks" and "callbacks still running" are always observed
        // together:
        //
        //   bits  0..31  tasks still pending
        //   bits 32..62  Decrement() calls that already took their share of
        //                the counter but have not finished their callbacks
        //   bit  63      COMPLETED: the last pending task has been decremented
        //                (stays set until Reset(); set by the same exchange
        //                that performs the last decrement)
        //
        // DONE <=> pending == 0 && in_flight == 0.
        static constexpr uint64_t PENDING_MASK = 0xFFFFFFFFull;
        static constexpr uint64_t IN_FLIGHT_UNIT = 1ull << 32;
        static constexpr uint64_t COMPLETED_BIT = 1ull << 63;
        static constexpr uint64_t BUSY_MASK = ~COMPLETED_BIT;

        static constexpr uint32_t Pending_of(uint64_t _state)
        {
            return static_cast< uint32_t >(_state & PENDING_MASK);
        }

        std::atomic< uint64_t > state;

        // Protects the condition-variable hand-off and every member below.
        // The state itself is atomic; the mutex guarantees that a waiter
        // cannot miss the notification sent between its predicate check and
        // its sleep, and that callbacks can be registered while tasks run.
        mutable std::mutex mutex;

        // Notified when the counter becomes DONE.
        mutable std::condition_variable cv;

        // Optional continuation invoked once, when the counter reaches zero.
        // Consumed (moved out) by the thread that completes the last task.
        std::function< void() > on_zero;
        bool on_zero_registered = false;

        // Optional progress callback invoked on every decrement with the
        // number of tasks still pending after that decrement. Held through a
        // shared_ptr so a decrementer can invoke it without holding the lock.
        std::shared_ptr< const std::function< void(uint32_t) > > on_decrement;
        std::atomic< bool > has_on_decrement{ false };

        // First exception reported for this group, if any.
        std::exception_ptr first_failure;

        // Marks one Decrement() as finished. The thread that makes the
        // counter DONE wakes every waiter.
        //
        // The final transition is performed while holding the mutex: a
        // waiter that returns from Wait() has necessarily taken the mutex
        // after this thread released it, so the counter can be destroyed as
        // soon as Wait() returns without this thread touching it afterwards.
        void Retire() noexcept
        {
            uint64_t current = state.load(std::memory_order_relaxed);

            // Fast path: someone else is still pending or in flight, so this
            // is not the transition to DONE and nobody has to be woken.
            while (((current - IN_FLIGHT_UNIT) & BUSY_MASK) != 0)
            {
                if (state.compare_exchange_weak(current, current - IN_FLIGHT_UNIT,
                    std::memory_order_release, std::memory_order_relaxed))
                {
                    return;
                }
            }

            std::lock_guard< std::mutex > lock(mutex);
            state.fetch_sub(IN_FLIGHT_UNIT, std::memory_order_acq_rel);
            cv.notify_all();
        }

    public:

        // Creates a counter with the given initial value, typically the
        // number of tasks that will decrement it.
        explicit Atomic_Counter(uint32_t _initial_value = 0)
            : state(_initial_value)
        {}

        // Neither copyable nor movable: worker threads hold the counter by
        // address (through Counter_Ptr) while they decrement it, so its
        // address must remain stable for its whole lifetime.
        //
        // Share counters through Counter_Ptr / Make_counter(). A counter
        // that lives on a stack frame is only safe if that frame waits with
        // Wait() / Wait_for() before returning; polling Is_done() is not
        // enough to know that the last decrementer left the object.
        Atomic_Counter(const Atomic_Counter&) = delete;
        Atomic_Counter& operator=(const Atomic_Counter&) = delete;
        Atomic_Counter(Atomic_Counter&&) = delete;
        Atomic_Counter& operator=(Atomic_Counter&&) = delete;

        // =========================================================
        // Core operations
        // =========================================================

        // Increments the counter by _amount. Must be called before the
        // additional tasks it accounts for are submitted, otherwise a
        // waiter could observe zero while those tasks are still pending.
        // A zero amount is a no-op.
        //
        // Throws std::overflow_error instead of wrapping around, and
        // std::logic_error if the group already completed (waiters may have
        // been released and the continuation has run: call Reset() to start
        // a new cycle). A counter that was created at zero and never
        // decremented has not completed, so the "create empty, then
        // Increment() once per task" pattern is fine.
        void Increment(uint32_t _amount = 1)
        {
            if (_amount == 0) return;

            uint64_t current = state.load(std::memory_order_relaxed);
            uint64_t desired;

            do
            {
                if ((current & COMPLETED_BIT) != 0)
                {
                    throw std::logic_error(
                        "Atomic_Counter::Increment: the counter already completed; "
                        "call Reset() to reuse it");
                }

                if (Pending_of(current) > std::numeric_limits< uint32_t >::max() - _amount)
                {
                    throw std::overflow_error(
                        "Atomic_Counter::Increment: the counter would overflow");
                }

                desired = current + _amount;
            }
            while (!state.compare_exchange_weak(current, desired,
                std::memory_order_acq_rel, std::memory_order_relaxed));
        }

        // Decrements the counter by one. Called by Task::Execute() when a
        // tracked task finishes. When the counter reaches zero, the on_zero
        // continuation runs on this thread and every thread blocked in
        // Wait() is woken after it returned.
        //
        // Throws std::logic_error if the counter is already zero: that means
        // a task completed twice or the counter was constructed too low, and
        // letting the value wrap around would deadlock every waiter.
        //
        // Memory ordering. The exchange is acquire-release: the release half
        // publishes everything this task wrote, and the acquire half makes
        // the thread that performs the LAST decrement see what every other
        // task of the group wrote, so the on_zero continuation can safely
        // consume the group's results. (Release alone would only order this
        // task's own writes towards waiters, not towards on_zero.)
        //
        // Exception safety. The counter is released even if a callback
        // throws: both callbacks always get their turn, waiters are always
        // woken, and the first exception is stored in the counter
        // (Report_failure) before waiters are released and then rethrown to
        // the caller of Decrement().
        void Decrement()
        {
            uint64_t current = state.load(std::memory_order_relaxed);
            uint64_t desired;

            do
            {
                if (Pending_of(current) == 0)
                {
                    throw std::logic_error(
                        "Atomic_Counter::Decrement: the counter is already zero "
                        "(a task completed twice, or the counter was constructed too low)");
                }

                // Take the task's share and register this call as in flight in
                // one step: the counter cannot look DONE before the
                // callbacks below have returned.
                desired = current - 1 + IN_FLIGHT_UNIT;

                if (Pending_of(current) == 1)
                {
                    desired |= COMPLETED_BIT;
                }
            }
            while (!state.compare_exchange_weak(current, desired,
                std::memory_order_acq_rel, std::memory_order_relaxed));

            const uint32_t remaining = Pending_of(desired);
            std::exception_ptr error;

            // Progress is reported before the continuation, so the
            // continuation observes every progress callback of its group.
            if (has_on_decrement.load(std::memory_order_acquire))
            {
                try
                {
                    std::shared_ptr< const std::function< void(uint32_t) > > progress;

                    {
                        std::lock_guard< std::mutex > lock(mutex);
                        progress = on_decrement;
                    }

                    if (progress)
                    {
                        (*progress)(remaining);
                    }
                }
                catch (...)
                {
                    error = std::current_exception();
                }
            }

            if (remaining == 0)
            {
                try
                {
                    // Claimed under the lock: either the continuation was
                    // registered before the group completed (we run it here)
                    // or it is registered later and Set_on_zero() runs it.
                    std::function< void() > continuation;

                    {
                        std::lock_guard< std::mutex > lock(mutex);
                        continuation = std::move(on_zero);
                        on_zero = nullptr;
                    }

                    // Outside the lock, on the thread that completed the
                    // last task.
                    if (continuation)
                    {
                        continuation();
                    }
                }
                catch (...)
                {
                    if (!error)
                    {
                        error = std::current_exception();
                    }
                }
            }

            // Stored BEFORE the waiters are released, so whoever wakes up can
            // already see that the group failed.
            if (error)
            {
                Report_failure(error);
            }

            Retire();

            if (error)
            {
                std::rethrow_exception(error);
            }
        }

        // Number of tasks still pending. Relaxed: intended for diagnostics
        // and sanity checks, the value may be stale by the time it is read.
        // Inside an on_decrement / on_zero callback it already excludes the
        // task that triggered the callback.
        uint32_t Get_count() const
        {
            return Pending_of(state.load(std::memory_order_relaxed));
        }

        // Returns true once the counter is DONE: no task is pending and no
        // Decrement() is still running callbacks.
        //
        // Acquire ordering, like Wait(): callers use a true result to start
        // reading what the tasks produced, so the load must synchronize
        // with the release in Decrement() / Retire(). A relaxed load would
        // let the caller observe zero before the tasks' writes are visible.
        //
        // Calling this (or Wait()) from inside an on_decrement / on_zero
        // callback of the same counter reports "not done yet": the callback
        // is, by definition, still running.
        bool Is_done() const
        {
            return (state.load(std::memory_order_acquire) & BUSY_MASK) == 0;
        }

        // =========================================================
        // Waiting
        // =========================================================

        // Blocks the calling thread until the counter is DONE. Never throws
        // for a failed task; call Rethrow_if_failed() afterwards.
        //
        // Do not call it from an on_decrement / on_zero callback of this same
        // counter (it would wait for itself).
        //
        // Avoid calling this from a worker thread: it parks the worker
        // without doing useful work. Prefer Thread_Dispatcher's
        // Submit_and_wait (which steals work) or an on_zero continuation.
        void Wait() const
        {
            // Always through the mutex (no lock-free shortcut): returning
            // from here then implies the thread that completed the counter
            // has released the mutex, so the counter may be destroyed.
            std::unique_lock< std::mutex > lock(mutex);
            cv.wait(lock, [this] { return Is_done(); });
        }

        // Non-blocking check, equivalent to Is_done(). Kept as a named
        // counterpart of Wait() for call sites that poll while doing other
        // work on the calling thread.
        bool Try_wait() const
        {
            return Is_done();
        }

        // Blocks until the counter is DONE or _timeout elapses.
        // Returns true if the counter was done within the timeout.
        // Useful to detect a task that never finishes, and for work with a
        // real deadline (network loads, remote connections...).
        bool Wait_for(std::chrono::milliseconds _timeout) const
        {
            std::unique_lock< std::mutex > lock(mutex);

            // The predicate protects against spurious wake-ups: the call
            // only returns true when the counter really is done.
            return cv.wait_for(lock, _timeout, [this] { return Is_done(); });
        }

        // =========================================================
        // Failures
        // =========================================================

        // Records the exception of a failed task (or callback) in the group.
        // The first failure wins, later ones are ignored; the counter does
        // not touch its value: the task still has to call Decrement().
        // Task::Execute() does both, in that order.
        void Report_failure(std::exception_ptr _error) noexcept
        {
            if (!_error) return;

            std::lock_guard< std::mutex > lock(mutex);

            if (!first_failure)
            {
                first_failure = std::move(_error);
            }
        }

        // True if any task of the group reported a failure.
        bool Has_failed() const
        {
            std::lock_guard< std::mutex > lock(mutex);
            return first_failure != nullptr;
        }

        // The first failure of the group, or an empty exception_ptr.
        std::exception_ptr Get_failure() const
        {
            std::lock_guard< std::mutex > lock(mutex);
            return first_failure;
        }

        // Rethrows the first failure of the group in the calling thread; does
        // nothing if there was none. Meant to be called after Wait() /
        // Is_done(): a failed task still counts as finished, so waiters are
        // never left blocked, and this is how they learn about the failure.
        // Every waiter sees it, it is not consumed.
        void Rethrow_if_failed() const
        {
            const std::exception_ptr failure = Get_failure();

            if (failure)
            {
                std::rethrow_exception(failure);
            }
        }

        // =========================================================
        // Callbacks (task chaining without blocking)
        // =========================================================

        // Sets a callback invoked on every decrement with the number of
        // tasks still pending (0 on the last one, right before on_zero).
        // Intended for progress bars and granular logging.
        //
        // Runs on the worker thread that performed the decrement, so it
        // must be short. Safe to set while tasks are running, but it only
        // sees the decrements that happen after it is set. At most once per
        // cycle: throws std::logic_error otherwise, and
        // std::invalid_argument for an empty function.
        void Set_on_decrement(std::function< void(uint32_t) > _callback)
        {
            if (!_callback)
            {
                throw std::invalid_argument(
                    "Atomic_Counter::Set_on_decrement: the callback is empty");
            }

            std::lock_guard< std::mutex > lock(mutex);

            if (on_decrement)
            {
                throw std::logic_error(
                    "Atomic_Counter::Set_on_decrement: a callback is already set");
            }

            on_decrement = std::make_shared< const std::function< void(uint32_t) > >(std::move(_callback));
            has_on_decrement.store(true, std::memory_order_release);
        }

        // Sets a continuation invoked when the counter reaches zero. Used to
        // chain work: when all dependencies finish, the continuation submits
        // the next task without any thread blocking.
        //
        // Runs on the worker thread that completed the last task, so it
        // must be short (typically just a Submit).
        //
        // It is never lost, whatever the interleaving with the tasks:
        //   - registered before the group completes: runs once, on the
        //     thread that completes the last task;
        //   - registered after the group completed (the tasks were faster
        //     than the caller): runs once, immediately, on the calling
        //     thread, before Set_on_zero() returns. An exception it throws
        //     propagates to the caller.
        // A counter that was never decremented (e.g. created at zero) has
        // not completed, so the continuation simply waits for its first
        // completion.
        //
        // At most once per cycle: throws std::logic_error otherwise, and
        // std::invalid_argument for an empty function.
        void Set_on_zero(std::function< void() > _callback)
        {
            if (!_callback)
            {
                throw std::invalid_argument(
                    "Atomic_Counter::Set_on_zero: the continuation is empty");
            }

            {
                std::lock_guard< std::mutex > lock(mutex);

                if (on_zero_registered)
                {
                    throw std::logic_error(
                        "Atomic_Counter::Set_on_zero: a callback is already set");
                }

                on_zero_registered = true;

                // The COMPLETED bit is set before the last decrementer takes
                // this lock to claim the continuation, so while the bit is
                // clear the decrementer is guaranteed to find what we store.
                if ((state.load(std::memory_order_acquire) & COMPLETED_BIT) == 0)
                {
                    on_zero = std::move(_callback);
                    return;
                }
            }

            // The group had already completed when the continuation was
            // registered. It was not stored, so the thread that completed the
            // group has nothing to run: this is the only place it runs, which
            // makes it exactly once.
            _callback();
        }

        // Resets the counter to a new value and clears both callbacks and
        // the recorded failure so the counter can be reused for another group
        // of tasks.
        // Only valid while the counter is DONE and no thread is waiting on
        // it or about to decrement it. Throws std::logic_error if the
        // counter is not done.
        void Reset(uint32_t _new_value)
        {
            std::lock_guard< std::mutex > lock(mutex);

            if (!Is_done())
            {
                throw std::logic_error(
                    "Atomic_Counter::Reset: the counter has not completed yet");
            }

            on_zero = nullptr;
            on_zero_registered = false;
            on_decrement = nullptr;
            has_on_decrement.store(false, std::memory_order_relaxed);
            first_failure = nullptr;

            state.store(_new_value, std::memory_order_release);
        }
    };

    // Counters are shared between the submitter (who waits on it) and the
    // tasks (which decrement it), so shared ownership is the natural model.
    using Counter_Ptr = std::shared_ptr< Atomic_Counter >;

    // Creates a shared Atomic_Counter with the given initial value.
    inline Counter_Ptr Make_counter(uint32_t _initial_value = 0)
    {
        return std::make_shared< Atomic_Counter >(_initial_value);
    }

}
