#pragma once

#include <Atomic_Counter.hpp>
#include <Circular_Queue.hpp>
#include <Task.hpp>

#include <span>
#include <thread>
#include <vector>

namespace ThreadDispatcher
{

    // Thread_Dispatcher: central thread pool of the Job System.
    //
    // Owns a fixed set of worker threads and a single central Circular_Queue
    // of Tasks. Workers block on Pop() when the queue is empty and execute
    // tasks as they arrive. The dispatcher itself is unaware of counters,
    // dependency graphs or chunk sizes — those concerns belong to the
    // callers (Parallel_For, Job_Graph, etc.).
    //
    // Lifetime: construct once at engine startup (via EngineCore), destroy
    // at shutdown. Not copyable or movable — workers hold a pointer to the
    // queue which must remain stable.
    class Thread_Dispatcher
    {


        // =========================================================
       // Data
       // =========================================================

       // Central task queue. Workers block on Pop(); the main thread
       // uses Try_pop() inside Steal_until_done() to avoid sleeping.
        Circular_Queue<Task> queue;

        // Worker threads. Spawned in the constructor, joined in Shutdown().
        std::vector<std::thread> workers;

        // Set to true by Shutdown(); checked in the destructor to avoid
        // calling Shutdown() twice.
        bool shutdown_called = false;

    public:

        // =========================================================
        // Lifecycle
        // =========================================================

        // Constructs the dispatcher and spawns worker threads.
        // _thread_count == 0  →  hardware_concurrency() - 1
        //   (reserves the main thread; minimum 1 worker).
        // _queue_capacity: maximum number of tasks in flight at once.
        //   If the queue is full, Submit() blocks until space is available.
        explicit Thread_Dispatcher(size_t _thread_count = 0,
            size_t _queue_capacity = 1024);

        // Calls Shutdown() if not already called.
        ~Thread_Dispatcher();

        Thread_Dispatcher(const Thread_Dispatcher&) = delete;
        Thread_Dispatcher& operator=(const Thread_Dispatcher&) = delete;
        Thread_Dispatcher(Thread_Dispatcher&&) = delete;
        Thread_Dispatcher& operator=(Thread_Dispatcher&&) = delete;

        // =========================================================
        // Submit — fire and forget / caller manages the counter
        // =========================================================

        // Enqueues a single task. Blocks if the queue is full.
        // The task must already carry its Counter_Ptr (if any).
        // Task is not copyable — caller must std::move it in.
        void Submit(Task _task);

        // Enqueues a contiguous span of tasks.
        // All tasks must already share the same Counter_Ptr (option A).
        // Returns the Counter_Ptr from the first task so the caller can
        // wait on it or chain continuations — returns nullptr if the
        // span is empty or the first task has no counter.
        // Blocks per-task if the queue fills up.
        Counter_Ptr Submit_group(std::span<Task> _tasks);

        // =========================================================
        // Submit + work-stealing wait
        // =========================================================

        // Overload A — caller already has a Task with a Counter_Ptr.
        // Precondition: _task.Has_counter() == true.
        // Steals work from the queue while waiting; does not sleep.
        // Intended for the main thread; avoid calling from worker threads.
        void Submit_and_wait(Task _task);

        // Overload B — convenience: accepts any void() callable directly.
        // Creates the Counter_Ptr internally; the caller never sees it.
        // Usage:  dispatcher.Submit_and_wait([&]{ do_work(); });
        template< typename CALLABLE >
        void Submit_and_wait(CALLABLE&& _callable)
        {
            static_assert(std::is_invocable_r_v< void, CALLABLE >,
                "Submit_and_wait callable must be invocable as void()");

            auto counter = Make_counter(1);
            Submit_and_wait( Make_task(std::forward< CALLABLE >(_callable),counter));
        }

        // Enqueues a group of tasks and waits for all to complete.
        // Uses the Counter_Ptr shared by the tasks to know when the
        // group is done; steals work while waiting (same as above).
        // All tasks must share the same Counter_Ptr (option A).
        // No-op if the span is empty.
        void Submit_group_and_wait(std::span<Task> _tasks);

        // =========================================================
        // Query
        // =========================================================

        // Number of worker threads owned by this dispatcher.
        size_t Thread_count() const;

        // Approximate number of tasks currently in the queue.
        // The value may be stale by the time it is read — use only
        // for diagnostics and profiling (e.g. Tracy stats overlay).
        size_t Pending_count() const;

        // =========================================================
        // Shutdown
        // =========================================================

        // Drains in-flight tasks, cancels the queue, joins all workers,
        // and marks the dispatcher as shut down.
        // Safe to call multiple times — subsequent calls are no-ops.
        // Must be called before destroying resources that queued tasks
        // may still reference (EngineCore controls the order).
        void Shutdown();

    private:

        // =========================================================
        // Internal helpers
        // =========================================================

        // Entry point for every worker thread.
        // Loops on queue.Pop() → task.Execute() until the queue is
        // cancelled (Shutdown), at which point Pop() returns nullopt
        // and the loop exits.
        void Worker_loop();

        // Steals and executes tasks from the queue until _counter reaches
        // zero or the queue is empty/cancelled.
        // Used by Submit_and_wait / Submit_group_and_wait to keep the
        // calling thread productive while its submitted tasks finish.
        void Steal_until_done(const Counter_Ptr& _counter);

       
    };

} // namespace JobSystem