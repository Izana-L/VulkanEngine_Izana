#pragma once

#include <Thread_Dispatcher.hpp>
#include <Task.hpp>
#include <Atomic_Counter.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
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

        // Builds all tasks for a Parallel_For call.
        // Takes a pointer to the callable (stable for the duration of
        // the call since Parallel_For is synchronous) to avoid copying
        // a potentially large capture list N times.
        //
        // All tasks share one counter initialised to the number of chunks;
        // Submit_group_and_wait() waits on it.
        //
        // Precondition: _chunk_size > 0 and _first < _last (Parallel_For
        // checks both; this function does not divide by a value it has not
        // seen validated).
        template< typename CALLABLE >
        std::vector< Task > Build_tasks(const CALLABLE* _callable,
                                                                size_t    _first,
                                                                size_t    _last,
                                                                size_t    _chunk_size)
        {
            // Compute number of chunks (ceiling division). Written as
            // quotient + remainder instead of (total + chunk - 1) / chunk:
            // that form wraps around for a huge _chunk_size and would
            // silently compute zero chunks, running nothing at all.
            const size_t total = _last - _first;
            const size_t num_chunks = total / _chunk_size + (total % _chunk_size != 0 ? 1 : 0);

            // The counter is 32-bit: a larger count would be truncated and
            // the group would be reported complete too early.
            if (num_chunks > std::numeric_limits< uint32_t >::max())
            {
                throw std::length_error(
                    "Parallel_For: the range yields more chunks than a counter can hold; "
                    "use a larger _chunk_size");
            }

            // One shared counter for the whole group.
            // Initialised to num_chunks: each task decrements once on finish.
            auto counter = Make_counter(
                static_cast<uint32_t>(num_chunks));

            std::vector< Task > tasks;
            tasks.reserve(num_chunks);

            for (size_t c = 0; c < num_chunks; ++c)
            {
                // c * _chunk_size < total, so this cannot overflow; and the
                // end is clamped by comparing the remaining length instead of
                // computing chunk_begin + _chunk_size, which could wrap.
                const size_t chunk_begin = _first + c * _chunk_size;
                const size_t chunk_end = (_last - chunk_begin > _chunk_size)
                    ? chunk_begin + _chunk_size
                    : _last;

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
    // Preconditions (checked in EVERY build, not only in debug):
    //   _chunk_size > 0   -> std::invalid_argument otherwise, even for an
    //                        empty range: the same bad call must not succeed
    //                        or fail depending on the data
    //   CALLABLE matches one of the two supported signatures (compile time)
    //
    // Edge cases:
    //   _first >= _last   → no-op, nothing enqueued
    //   range < _chunk_size → single task covering the whole range
    //   range % _chunk_size != 0 → last chunk is smaller, clamped to _last
    //   any _chunk_size, up to SIZE_MAX, covers the range exactly once
    //
    // Exceptions:
    //   If a chunk throws, the other chunks still run to completion (they
    //   reference this call's callable, which lives on the caller's stack),
    //   and then the FIRST exception raised by a chunk of this call is
    //   rethrown here - whether the chunk ran on a worker or on the calling
    //   thread. Failures of unrelated tasks that the calling thread happens
    //   to steal while waiting are never rethrown here.
    //   std::runtime_error if the dispatcher was shut down: nothing has been
    //   enqueued in that case, so the callable is never invoked.
    //
    // Note on nested Parallel_For:
    //   Calling Parallel_For from inside a task lambda is safe (no
    //   deadlock). Two properties make that true: waiting is done by
    //   stealing work, never by sleeping, and submitting never sleeps
    //   either - when the queue is full the submitting thread runs queued
    //   tasks until there is room (see Thread_Dispatcher::Submit). Hence
    //   even if every worker is inside a task that submits a nested
    //   Parallel_For at the same time, the queue keeps draining.
    //   Stolen and helped tasks execute on the waiting thread's stack, so
    //   very deep nesting grows that stack; and a waiting thread may run an
    //   unrelated long task before it notices its own group is done.
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

        // A real check, not an assert: with assertions compiled out
        // (Release) a zero chunk size would divide by zero in Build_tasks().
        if (_chunk_size == 0)
        {
            throw std::invalid_argument("Parallel_For: _chunk_size must be > 0");
        }

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