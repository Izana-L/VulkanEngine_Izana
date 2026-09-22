#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace ThreadDispatcher
{

    // Circular_Queue: a fixed-capacity FIFO queue designed for use as a
    // producer/consumer buffer between threads. Handles synchronization
    // internally, blocking Push/Emplace when full and Pop when empty.
    //
    // Allocates a single contiguous, correctly aligned buffer on
    // construction and never reallocates at runtime. Elements are
    // constructed in place when pushed and destroyed when popped; any
    // element still stored when the queue is destroyed is destroyed then.
    //
    // Lifecycle:
    //   - Close():  no further pushes are accepted; pops keep returning the
    //               remaining elements and report "finished" (nullopt) once
    //               the queue is empty. This is the orderly shutdown path.
    //   - Cancel(): no further pushes are accepted and every pop returns
    //               nullopt at once, whatever is still stored. This is the
    //               abort path; the stored elements are destroyed by the
    //               destructor.
    //
    // Thread-safe: every public operation takes the internal mutex, the
    // flags included, so a waiter can never miss a wake-up between testing
    // its predicate and blocking on the condition variable.
    template< typename TYPE >
    class Circular_Queue final
    {
    public:

        using Value_Type = TYPE;

    private:

        struct Storage_Deleter
        {
            size_t capacity;

            void operator()(Value_Type* _slots) const
            {
                std::allocator< Value_Type >{}.deallocate(_slots, capacity);
            }
        };

        // Raw slots: constructed only while occupied.
        std::unique_ptr< Value_Type[], Storage_Deleter > slots;
        const size_t                                     capacity;

        size_t head = 0;      // index of the front element
        size_t count = 0;     // occupied slots

        bool closed = false;
        bool cancelled = false;

        mutable std::mutex      mutex;
        std::condition_variable not_empty;
        std::condition_variable not_full;

        Value_Type* Slot(size_t _logical_index)
        {
            return slots.get() + (head + _logical_index) % capacity;
        }

        // Precondition: mutex held, count < capacity, not closed/cancelled.
        template< typename... ARGUMENTS >
        void Construct_back(ARGUMENTS&&... _arguments)
        {
            ::new (static_cast<void*>(Slot(count))) Value_Type(std::forward< ARGUMENTS >(_arguments)...);
            ++count;
        }

        // Precondition: mutex held, count > 0.
        Value_Type Take_front()
        {
            Value_Type* front = Slot(0);
            Value_Type  value = std::move(*front);
            front->~Value_Type();

            head = (head + 1) % capacity;
            --count;

            return value;
        }

        // Shared body of Push / Emplace. Returns false when the queue no
        // longer accepts elements.
        template< typename... ARGUMENTS >
        bool Enqueue(ARGUMENTS&&... _arguments)
        {
            std::unique_lock lock(mutex);

            not_full.wait(lock, [this] { return count < capacity || closed || cancelled; });

            if (closed || cancelled) return false;

            Construct_back(std::forward< ARGUMENTS >(_arguments)...);

            lock.unlock();
            not_empty.notify_one();

            return true;
        }

    public:

        // Constructs the queue with the given capacity (at least 1).
        explicit Circular_Queue(size_t _desired_capacity)
            : slots(std::allocator< Value_Type >{}.allocate(_desired_capacity == 0 ? 1 : _desired_capacity),
                Storage_Deleter{ _desired_capacity == 0 ? 1 : _desired_capacity }),
            capacity(_desired_capacity == 0 ? 1 : _desired_capacity)
        {}

        // Destroys every element still stored. Callers must make sure no
        // thread is blocked in Push/Pop at this point (Thread_Dispatcher
        // joins its workers before the queue goes away).
        ~Circular_Queue()
        {
            for (size_t i = 0; i < count; ++i)
                Slot(i)->~Value_Type();
        }

        // Not copyable or movable: owns a live buffer with potentially
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
            return capacity;
        }

        // Current number of elements in the queue. May be stale by the time
        // the caller reads it; meant for diagnostics.
        size_t Size() const
        {
            std::lock_guard lock(mutex);
            return count;
        }

        bool Is_empty() const
        {
            return Size() == 0;
        }

        bool Is_full() const
        {
            return Size() == capacity;
        }

        // =========================================================
        // Producer operations
        // =========================================================

        // Constructs an element in place at the back of the queue.
        // Blocks while the queue is full. Returns false, without storing
        // anything, once the queue has been closed or cancelled.
        template< typename... ARGUMENTS >
        bool Emplace(ARGUMENTS&&... _arguments)
        {
            return Enqueue(std::forward< ARGUMENTS >(_arguments)...);
        }

        // Copies an element to the back of the queue. Same blocking and
        // return semantics as Emplace.
        bool Push(const Value_Type& _value)
        {
            return Enqueue(_value);
        }

        // Moves an element to the back of the queue. Same blocking and
        // return semantics as Emplace.
        bool Push(Value_Type&& _value)
        {
            return Enqueue(std::move(_value));
        }

        // =========================================================
        // Consumer operations
        // =========================================================

        // Removes and returns the front element.
        // Blocks while the queue is empty and still open. Returns nullopt
        // once the queue is cancelled, or closed and drained.
        std::optional< Value_Type > Pop()
        {
            std::unique_lock lock(mutex);

            not_empty.wait(lock, [this] { return count > 0 || closed || cancelled; });

            if (cancelled || count == 0) return std::nullopt;

            std::optional< Value_Type > value(Take_front());

            lock.unlock();
            not_full.notify_one();

            return value;
        }

        // Non-blocking pop: returns the front element if available,
        // or nullopt immediately if the queue is empty or cancelled.
        // Used by threads that want to check for work without blocking
        // (work stealing, yielding instead of sleeping).
        std::optional< Value_Type > Try_pop()
        {
            std::unique_lock lock(mutex);

            if (cancelled || count == 0) return std::nullopt;

            std::optional< Value_Type > value(Take_front());

            lock.unlock();
            not_full.notify_one();

            return value;
        }

        // =========================================================
        // Shutdown
        // =========================================================

        // Orderly shutdown: rejects further pushes, lets consumers drain
        // what is stored, then makes Pop() return nullopt.
        void Close()
        {
            {
                std::lock_guard lock(mutex);
                closed = true;
            }

            not_empty.notify_all();
            not_full.notify_all();
        }

        // Abort: rejects further pushes and makes every Pop() return
        // nullopt immediately, leaving the stored elements to the
        // destructor. Anything waiting on the completion of those elements
        // will never be signalled; see Thread_Dispatcher::Steal_until_done.
        void Cancel()
        {
            {
                std::lock_guard lock(mutex);
                cancelled = true;
            }

            not_empty.notify_all();
            not_full.notify_all();
        }

        bool Is_closed() const
        {
            std::lock_guard lock(mutex);
            return closed;
        }

        bool Is_cancelled() const
        {
            std::lock_guard lock(mutex);
            return cancelled;
        }
    };

}
