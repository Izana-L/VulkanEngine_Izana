#pragma once

#include <Atomic_Counter.hpp>
#include <Circular_Queue.hpp>
#include <Task.hpp>

#include <atomic>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace ThreadDispatcher
{

    // Thread_Dispatcher: central thread pool of the Job System.
    //
    // Owns a fixed set of worker threads and a single central Circular_Queue
    // of Tasks. Workers block on Pop() when the queue is empty and execute
    // tasks as they arrive. The dispatcher itself is unaware of counters,
    // dependency graphs or chunk sizes; those concerns belong to the
    // callers (Parallel_For, Job_Graph, etc.).
    //
    // Lifetime: construct once at engine startup (via EngineCore), destroy
    // at shutdown. Not copyable or movable: workers hold a pointer to the
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

        // Set by the first Shutdown() call; makes every later call a no-op
        // and every later Submit() an error.
        std::atomic<bool> shutdown_called{ false };

        // Number of tasks whose callable threw while running on a worker
        // thread. Such failures are logged by the worker and counted here
        // so the application can detect them; see Worker_loop().
        std::atomic<size_t> failed_task_count{ 0 };

    public:

        // =========================================================
        // Lifecycle
        // =========================================================

        // Constructs the dispatcher and spawns worker threads.
        // _thread_count == 0  ->  hardware_concurrency() - 1
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
        // Submit: fire and forget / caller manages the counter
        // =========================================================

        // Enqueues a single task. Blocks if the queue is full.
        // The task must already carry its Counter_Ptr (if any).
        // Task is not copyable; caller must std::move it in.
        // Throws std::invalid_argument for an empty task (one that was
        // default-constructed or already moved from), in every build.
        // Throws std::runtime_error after Shutdown(): a task accepted then
        // would never run and its counter would never reach zero.
        void Submit(Task _task);

        // Enqueues a contiguous span of tasks.
        // All tasks must already share the same Counter_Ptr (option A);
        // the whole span is validated before any task is moved into the
        // queue, and std::invalid_argument is thrown if a task is empty or
        // carries a different counter than the first one.
        // Returns the Counter_Ptr from the first task so the caller can
        // wait on it or chain continuations; returns nullptr if the
        // span is empty or the tasks have no counter.
        // Blocks per-task if the queue fills up.
        Counter_Ptr Submit_group(std::span<Task> _tasks);

        // =========================================================
        // Submit + work-stealing wait
        // =========================================================

        // Overload A: caller already has a Task with a Counter_Ptr.
        // Throws std::invalid_argument if the task is empty or carries no
        // counter (without one there is nothing to wait for).
        // Steals work from the queue while waiting; does not sleep.
        // Intended for the main thread; avoid calling from worker threads.
        // If a task executed on the calling thread throws, the wait still
        // runs to completion and the first such exception is rethrown
        // afterwards (see Steal_until_done).
        void Submit_and_wait(Task _task);

        // Overload B: convenience, accepts any void() callable directly.
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
        // All tasks must share the same non-null Counter_Ptr (option A);
        // std::invalid_argument is thrown otherwise, before any task is
        // moved into the queue. No-op if the span is empty.
        void Submit_group_and_wait(std::span<Task> _tasks);

        // =========================================================
        // Query
        // =========================================================

        // Number of worker threads owned by this dispatcher.
        size_t Thread_count() const;

        // Approximate number of tasks currently in the queue.
        // The value may be stale by the time it is read; use only
        // for diagnostics and profiling (e.g. Tracy stats overlay).
        size_t Pending_count() const;

        // True once Shutdown() has been called.
        bool Is_shut_down() const;

        // Number of tasks that threw while running on a worker thread since
        // the dispatcher was created. Non-zero means at least one failure
        // was logged to stderr by Worker_loop().
        size_t Failed_task_count() const;

        // =========================================================
        // Shutdown
        // =========================================================

        // Stops accepting tasks, lets the workers DRAIN every task already
        // queued (so every counter those tasks carry reaches zero), then
        // joins all workers. Safe to call multiple times and from any
        // thread except a worker; subsequent calls are no-ops.
        // Must be called before destroying resources that queued tasks
        // may still reference (EngineCore controls the order).
        void Shutdown();

    private:

        // =========================================================
        // Internal helpers
        // =========================================================

        // Entry point for every worker thread.
        // Loops on queue.Pop() -> task.Execute() until the queue reports
        // it is finished (closed and drained), at which point Pop()
        // returns nullopt and the loop exits.
        // A task that throws does not end the worker: an exception escaping
        // a std::thread terminates the whole process, so the failure is
        // logged to stderr and counted in failed_task_count instead. The
        // task's counter has already been released by Task::Execute(), so
        // no waiter is left blocked by the failure.
        void Worker_loop();

        // Steals and executes tasks from the queue until _counter reaches
        // zero. Yields while the queue is empty and other workers are still
        // running tasks. Returns early only if the queue was cancelled,
        // since a cancelled queue drops tasks and the counter might then
        // never reach zero.
        // A stolen task that throws does not abort the wait: the rest of
        // the group is still running on the workers, and returning early
        // would let the caller release data those tasks still reference.
        // The first exception is kept and rethrown once the group is done.
        // Used by Submit_and_wait / Submit_group_and_wait to keep the
        // calling thread productive while its submitted tasks finish.
        void Steal_until_done(const Counter_Ptr& _counter);

        // Enqueues one task or throws if the dispatcher no longer accepts work.
        void Enqueue_or_throw(Task&& _task);
    };

} // namespace ThreadDispatcher
