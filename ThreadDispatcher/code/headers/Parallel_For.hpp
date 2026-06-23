#pragma once

#include <Thread_Dispatcher.hpp>
#include <Task.hpp>
#include <Atomic_Counter.hpp>

#include <algorithm>
#include <cassert>
#include <span>
#include <type_traits>
#include <vector>

namespace ThreadDispatcher
{

    inline constexpr size_t DEFAULT_CHUNK_SIZE = 256;

    namespace Detail
    {
        // =========================================================
        // Callable trait detection
        // =========================================================

        // Detects whether CALLABLE accepts (size_t begin, size_t end).
        // Used to dispatch at compile time between the index overload
        // void(size_t i) and the range overload void(size_t, size_t).
        template< typename CALLABLE >
        inline constexpr bool is_range_callable = std::is_invocable_r_v< void, CALLABLE, size_t, size_t >;

        template< typename CALLABLE >
        inline constexpr bool is_index_callable = std::is_invocable_r_v< void, CALLABLE, size_t >;

        // =========================================================
        // Chunk builder
        // =========================================================

        // Builds and submits all tasks for a Parallel_For call.
        // Takes a pointer to the callable (stable for the duration of
        // the call since Parallel_For is synchronous) to avoid copying
        // a potentially large capture list N times.
        //
        // Returns the shared Counter_Ptr so the caller can wait on it
        // via Submit_group_and_wait.
        template< typename CALLABLE >
        std::vector< Task > Build_tasks(const CALLABLE* _callable,
                                                                size_t    _first,
                                                                size_t    _last,
                                                                size_t    _chunk_size)
        {
            // Compute number of chunks (ceiling division).
            const size_t total = _last - _first;
            const size_t num_chunks = (total + _chunk_size - 1) / _chunk_size;

            // One shared counter for the whole group.
            // Initialised to num_chunks: each task decrements once on finish.
            auto counter = Make_counter(
                static_cast<uint32_t>(num_chunks));

            std::vector< Task > tasks;
            tasks.reserve(num_chunks);

            for (size_t c = 0; c < num_chunks; ++c)
            {
                const size_t chunk_begin = _first + c * _chunk_size;
                const size_t chunk_end = std::min(chunk_begin + _chunk_size, _last);

                if constexpr (is_range_callable< CALLABLE >)
                {
                    // Range callable: void(size_t begin, size_t end)
                    // Capture begin/end by value; callable by pointer (no copy).
                    tasks.emplace_back(
                        Make_task([_callable, chunk_begin, chunk_end]()
                        {
                            (*_callable)(chunk_begin, chunk_end);
                        }, counter));
                           
                            
                }
                else
                {
                    // Index callable: void(size_t i)
                    // Expand the range into individual index calls inside
                    // the task — one task per chunk, not one per element.
                    tasks.emplace_back(
                        Make_task([_callable, chunk_begin, chunk_end]()
                            {
                                for (size_t i = chunk_begin; i < chunk_end; ++i)
                                    (*_callable)(i);
                            },counter));
                            
                            
                }
            }

            return tasks;
        }

    } // namespace Detail

    // =========================================================
    // Parallel_For
    // =========================================================

    // Splits [_first, _last) into chunks of _chunk_size elements and
    // executes them in parallel using _dispatcher's worker threads.
    // The calling thread steals chunks from the queue while waiting,
    // so it does useful work instead of sleeping.
    // Returns only when ALL chunks have finished executing.
    //
    // CALLABLE may be either:
    //   void(size_t i)                  — called once per index
    //   void(size_t begin, size_t end)  — called once per chunk
    // Detected automatically at compile time via is_invocable.
    //
    // Preconditions (asserted in debug):
    //   _chunk_size > 0
    //   CALLABLE matches one of the two supported signatures
    //
    // Edge cases:
    //   _first >= _last   → no-op, nothing enqueued
    //   range < _chunk_size → single task covering the whole range
    //   range % _chunk_size != 0 → last chunk is smaller, clamped to _last
    //
    // Note on nested Parallel_For:
    //   Calling Parallel_For from inside a task lambda is safe (no deadlock)
    //   because no thread ever sleeps while waiting — all waiting is done
    //   by stealing work. However, deeply nested calls may starve outer
    //   chunks if all workers are busy with inner tasks. Avoid nesting
    //   more than one level deep until work-stealing per-thread deques
    //   are introduced.
    template< typename CALLABLE >
    void Parallel_For(Thread_Dispatcher& _dispatcher,
        size_t             _first,
        size_t             _last,
        CALLABLE&& _callable,
        size_t             _chunk_size = DEFAULT_CHUNK_SIZE)
    {
        static_assert(
            Detail::is_range_callable< CALLABLE > ||
            Detail::is_index_callable< CALLABLE >,
            "Parallel_For: CALLABLE must be void(size_t) or "
            "void(size_t begin, size_t end)");

        assert(_chunk_size > 0 && "Parallel_For: _chunk_size must be > 0");

        // Nothing to do.
        if (_first >= _last) return;

        // Build all tasks sharing one counter, then hand them to the
        // dispatcher which will steal-wait until they all finish.
        // The callable is passed by pointer — it lives on the caller's
        // stack and Parallel_For does not return until all tasks are done,
        // so the pointer is always valid during task execution.
        auto tasks = Detail::Build_tasks(&_callable, _first, _last, _chunk_size);

        _dispatcher.Submit_group_and_wait(
            std::span< Task >(tasks));
    }

} 