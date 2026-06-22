#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

namespace ThreadDispatcher
{

    // Circular_Queue: a fixed-capacity FIFO queue designed for use as a
    // producer/consumer buffer between threads. Handles synchronization
    // internally, blocking Push/Emplace when full and Pop when empty.
    //
    // Allocates a single contiguous memory buffer on construction and
    // never reallocates at runtime, making it safe and cache-friendly
    // for high-frequency task queuing.
    //
    // Thread-safe: all public operations are protected by an internal
    // mutex and condition variable.
    template< typename TYPE >
    class Circular_Queue final
    {
    public:

        using Value_Type = TYPE;

    private:

        // Single contiguous buffer storing all elements as raw bytes.
        // Elements are constructed in place via placement new and
        // destroyed manually, avoiding default construction overhead.
        std::vector< std::byte > elements;
        const size_t             real_capacity;

        std::atomic< size_t >    first;
        std::atomic< size_t >    last;
        std::atomic< size_t >    allocated;

        // When cancelled, all blocking Push/Pop calls wake up and
        // return immediately (Pop returns nullopt). Used during shutdown
        // to unblock waiting worker threads.
        std::atomic< bool >      cancelled;
        std::condition_variable  condition;
        std::mutex               mutex;

    public:

        // Constructs the queue with the given capacity.
        // Allocates (capacity + 1) slots internally to distinguish
        // full from empty without an extra flag.
        explicit Circular_Queue(size_t _desired_capacity)
            : elements((_desired_capacity + 1) * sizeof(Value_Type)),
            real_capacity(_desired_capacity + 1),
            first(0),
            last(0),
            allocated(0),
            cancelled(false)
        {}

        // Not copyable or movable - owns a live buffer with potentially
        // constructed elements and blocking threads waiting on it.
        Circular_Queue(const Circular_Queue&) = delete;
        Circular_Queue& operator=(const Circular_Queue&) = delete;
        Circular_Queue(Circular_Queue&&) = delete;
        Circular_Queue& operator=(Circular_Queue&&) = delete;

        // =========================================================
        // Capacity
        // =========================================================

        // Maximum number of elements this queue can hold.
        size_t Capacity() const
        {
            return real_capacity - 1;
        }

        // Current number of elements in the queue.
        size_t Size() const
        {
            return allocated;
        }

        // Number of additional elements that can be pushed before blocking.
        size_t Available() const
        {
            return Capacity() - Size();
        }

        bool Is_empty() const
        {
            return Size() == 0;
        }

        bool Is_full() const
        {
            return Available() == 0;
        }

        // =========================================================
        // Element access
        // =========================================================

        // Returns a reference to the front element (next to be popped).
        // Precondition: !Is_empty()
        Value_Type& Front()
        {
            assert(!Is_empty() && "Front() called on an empty Circular_Queue");
            return Element_at(first);
        }

        const Value_Type& Front() const
        {
            assert(!Is_empty() && "Front() const called on an empty Circular_Queue");
            return Element_at(first);
        }

        // =========================================================
        // Producer operations
        // =========================================================

        // Constructs an element in place at the back of the queue.
        // Blocks if the queue is full until space becomes available
        // or Cancel() is called.
        template< typename... ARGUMENTS >
        void Emplace(ARGUMENTS&&... _arguments)
        {
            assert(Capacity() > 0 && "Emplace() called on a zero-capacity Circular_Queue");

            std::unique_lock lock(mutex);

            if (Is_full() && !cancelled)
            {
                condition.wait(lock, [this] { return !Is_full() || cancelled; });
            }

            if (cancelled) return;

            new (&Allocate_one()) Value_Type(std::forward< ARGUMENTS >(_arguments)...);

            lock.unlock();
            condition.notify_one();
        }

        // Copies an element to the back of the queue.
        // Blocks if the queue is full until space becomes available
        // or Cancel() is called.
        void Push(const Value_Type& _value)
        {
            assert(Capacity() > 0 && "Push() called on a zero-capacity Circular_Queue");

            std::unique_lock lock(mutex);

            if (Is_full() && !cancelled)
            {
                condition.wait(lock, [this] { return !Is_full() || cancelled; });
            }

            if (cancelled) return;

            Allocate_one() = _value;

            lock.unlock();
            condition.notify_one();
        }

        // Moves an element to the back of the queue.
        // Blocks if the queue is full until space becomes available
        // or Cancel() is called.
        void Push(Value_Type&& _value)
        {
            assert(Capacity() > 0 && "Push() called on a zero-capacity Circular_Queue");

            std::unique_lock lock(mutex);

            if (Is_full() && !cancelled)
            {
                condition.wait(lock, [this] { return !Is_full() || cancelled; });
            }

            if (cancelled) return;

            Allocate_one() = std::move(_value);

            lock.unlock();
            condition.notify_one();
        }

        // =========================================================
        // Consumer operations
        // =========================================================

        // Removes and returns the front element.
        // Blocks if the queue is empty until an element is available
        // or Cancel() is called (returns nullopt in that case).
        std::optional< Value_Type > Pop()
        {
            std::unique_lock lock(mutex);

            if (Is_empty() && !cancelled)
            {
                condition.wait(lock, [this] { return !Is_empty() || cancelled; });
            }

            if (cancelled) return std::nullopt;

            Value_Type value = std::move(Front());
            Free_one();

            lock.unlock();
            condition.notify_one();

            return value;
        }

        // Non-blocking pop: returns the front element if available,
        // or nullopt immediately if the queue is empty or cancelled.
        // Used by worker threads that want to check for work without
        // blocking (e.g. to do work-stealing or yield the CPU instead).
        std::optional< Value_Type > Try_pop()
        {
            std::unique_lock lock(mutex);

            if (Is_empty() || cancelled) return std::nullopt;

            Value_Type value = std::move(Front());
            Free_one();

            lock.unlock();
            condition.notify_one();

            return value;
        }

        // =========================================================
        // Shutdown
        // =========================================================

        // Wakes up all threads blocked on Push or Pop and causes them
        // to return immediately (Push is a no-op, Pop returns nullopt).
        // Used during Thread_Dispatcher shutdown to unblock workers
        // waiting for tasks that will never arrive.
        void Cancel()
        {
            cancelled = true;
            condition.notify_all();
        }

        // Returns true if Cancel() has been called.
        bool Is_cancelled() const
        {
            return cancelled;
        }

    private:

        // =========================================================
        // Internal helpers
        // =========================================================

        Value_Type& Element_at(size_t _index)
        {
            return reinterpret_cast<Value_Type&>(elements[_index * sizeof(Value_Type)]);
        }

        const Value_Type& Element_at(size_t _index) const
        {
            return reinterpret_cast<const Value_Type&>(elements[_index * sizeof(Value_Type)]);
        }

        // Reserves a slot at the back of the circular buffer and returns
        // a reference to it. Does not construct the element - caller is
        // responsible for constructing via placement new or assignment.
        Value_Type& Allocate_one()
        {
            if (last >= first)
            {
                if (last == real_capacity - 1)
                {
                    if (first > 0) return Allocate_one_before(0);
                }
                else
                {
                    return Allocate_one_before(last + 1);
                }
            }
            else
            {
                if (last < first - 1) return Allocate_one_before(last + 1);
            }

            throw std::bad_alloc();
        }

        Value_Type& Allocate_one_before(size_t _new_last)
        {
            size_t previous_last = last;
            last = _new_last;
            ++allocated;
            return Element_at(previous_last);
        }

        // Destroys the front element and advances the read pointer.
        void Free_one()
        {
            Front().~Value_Type();

            if (first < last)
            {
                ++first;
                --allocated;
            }
            else if (first > last)
            {
                if (++first == real_capacity) first = 0;
                --allocated;
            }
        }

    };

}