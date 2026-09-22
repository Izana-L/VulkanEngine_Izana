#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

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
    class Atomic_Counter
    {

    private:

        // Number of tasks still pending. Modified with atomic read-modify-
        // write operations from any number of worker threads at once.
        std::atomic< uint32_t > count;

        // Protects the condition-variable hand-off. The value itself is
        // atomic; the mutex only guarantees that a waiter cannot miss the
        // notification sent between its predicate check and its sleep.
        mutable std::mutex mutex;

        // Notified when the counter reaches zero.
        mutable std::condition_variable cv;

        // Optional continuation invoked once, when the counter reaches zero.
        std::function< void() > on_zero;

        // Optional progress callback invoked on every decrement with the
        // number of tasks still pending after that decrement.
        std::function< void(uint32_t) > on_decrement;

    public:

        // Creates a counter with the given initial value, typically the
        // number of tasks that will decrement it.
        explicit Atomic_Counter(uint32_t _initial_value = 0)
            : count(_initial_value)
        {}

        // Neither copyable nor movable: worker threads hold the counter by
        // address (through Counter_Ptr) while they decrement it, so its
        // address must remain stable for its whole lifetime.
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
        // A zero amount is a no-op. Throws std::overflow_error instead of
        // wrapping around.
        void Increment(uint32_t _amount = 1)
        {
            if (_amount == 0) return;

            uint32_t current = count.load(std::memory_order_relaxed);

            do
            {
                if (current > std::numeric_limits< uint32_t >::max() - _amount)
                {
                    throw std::overflow_error(
                        "Atomic_Counter::Increment: the counter would overflow");
                }
            }
            while (!count.compare_exchange_weak(current, current + _amount,
                std::memory_order_relaxed, std::memory_order_relaxed));
        }

        // Decrements the counter by one. Called by Task::Execute() when a
        // tracked task finishes. When the counter reaches zero, every thread
        // blocked in Wait() is woken and the on_zero continuation runs.
        //
        // Throws std::logic_error if the counter is already zero: that means
        // a task completed twice or the counter was constructed too low, and
        // letting the value wrap around would deadlock every waiter.
        //
        // Release ordering on the successful exchange pairs with the acquire
        // loads in Is_done() / Wait(): everything the task wrote before
        // finishing is visible to whoever observes the counter at zero.
        void Decrement()
        {
            uint32_t current = count.load(std::memory_order_relaxed);

            do
            {
                if (current == 0)
                {
                    throw std::logic_error(
                        "Atomic_Counter::Decrement: the counter is already zero "
                        "(a task completed twice, or the counter was constructed too low)");
                }
            }
            while (!count.compare_exchange_weak(current, current - 1,
                std::memory_order_release, std::memory_order_relaxed));

            const uint32_t remaining = current - 1;

            // Progress is reported before the waiters are released, so a
            // main thread woken by Wait() never sees a stale progress value.
            if (on_decrement)
            {
                on_decrement(remaining);
            }

            if (remaining == 0)
            {
                // Taking the mutex before notifying guarantees that a waiter
                // which has already checked the predicate is inside
                // cv.wait() when the notification is sent.
                {
                    std::lock_guard< std::mutex > lock(mutex);
                    cv.notify_all();
                }

                // The continuation runs outside the lock, on the thread that
                // completed the last task.
                if (on_zero)
                {
                    on_zero();
                }
            }
        }

        // Returns the current value. Relaxed: intended for diagnostics, the
        // value may be stale by the time it is read.
        uint32_t Get_count() const
        {
            return count.load(std::memory_order_relaxed);
        }

        // Returns true once the counter has reached zero.
        //
        // Acquire ordering, like Wait(): callers use a true result to start
        // reading what the tasks produced, so the load must synchronize
        // with the release in Decrement(). A relaxed load would let the
        // caller observe zero before the tasks' writes are visible.
        bool Is_done() const
        {
            return count.load(std::memory_order_acquire) == 0;
        }

        // =========================================================
        // Waiting
        // =========================================================

        // Blocks the calling thread until the counter reaches zero.
        //
        // Avoid calling this from a worker thread: it parks the worker
        // without doing useful work. Prefer Thread_Dispatcher's
        // Submit_and_wait (which steals work) or an on_zero continuation.
        void Wait() const
        {
            if (count.load(std::memory_order_acquire) == 0) return;

            std::unique_lock< std::mutex > lock(mutex);
            cv.wait(lock, [this]
                {
                    return count.load(std::memory_order_acquire) == 0;
                });
        }

        // Non-blocking check, equivalent to Is_done(). Kept as a named
        // counterpart of Wait() for call sites that poll while doing other
        // work on the calling thread.
        bool Try_wait() const
        {
            return count.load(std::memory_order_acquire) == 0;
        }

        // Blocks until the counter reaches zero or _timeout elapses.
        // Returns true if the counter reached zero within the timeout.
        // Useful to detect a task that never finishes, and for work with a
        // real deadline (network loads, remote connections...).
        bool Wait_for(std::chrono::milliseconds _timeout) const
        {
            if (count.load(std::memory_order_acquire) == 0) return true;

            std::unique_lock< std::mutex > lock(mutex);

            // The predicate protects against spurious wake-ups: the call
            // only returns true when the counter really reached zero.
            return cv.wait_for(lock, _timeout, [this]
                {
                    return count.load(std::memory_order_acquire) == 0;
                });
        }

        // =========================================================
        // Callbacks (task chaining without blocking)
        // =========================================================

        // Sets a callback invoked on every decrement with the number of
        // tasks still pending (0 on the last one, right before on_zero).
        // Intended for progress bars and granular logging.
        //
        // Runs on the worker thread that performed the decrement, so it
        // must be short. Must be set before any tracked task can finish,
        // and at most once per cycle. Throws std::logic_error otherwise.
        void Set_on_decrement(std::function< void(uint32_t) > _callback)
        {
            if (on_decrement)
            {
                throw std::logic_error(
                    "Atomic_Counter::Set_on_decrement: a callback is already set");
            }

            on_decrement = std::move(_callback);
        }

        // Sets a continuation invoked when the counter reaches zero. Used to
        // chain work: when all dependencies finish, the continuation submits
        // the next task without any thread blocking.
        //
        // Runs on the worker thread that completed the last task, so it
        // must be short (typically just a Submit). Must be set before any
        // tracked task can finish, and at most once per cycle. Throws
        // std::logic_error otherwise.
        void Set_on_zero(std::function< void() > _callback)
        {
            if (on_zero)
            {
                throw std::logic_error(
                    "Atomic_Counter::Set_on_zero: a callback is already set");
            }

            on_zero = std::move(_callback);
        }

        // Resets the counter to a new value and clears both callbacks so
        // the counter can be reused for another group of tasks.
        // Only valid while the counter is at zero and no thread is waiting
        // on it or about to decrement it. Throws std::logic_error if the
        // counter is not at zero.
        void Reset(uint32_t _new_value)
        {
            if (!Is_done())
            {
                throw std::logic_error(
                    "Atomic_Counter::Reset: the counter has not reached zero yet");
            }

            on_zero = nullptr;
            on_decrement = nullptr;

            count.store(_new_value, std::memory_order_relaxed);
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
