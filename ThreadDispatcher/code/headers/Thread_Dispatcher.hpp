#pragma once

#include <Atomic_Counter.hpp>
#include <Circular_Queue.hpp>
#include <Task.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
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

        // ---- Shutdown state ----------------------------------------

        // Set by the first Shutdown() call (under gate_mutex); makes every
        // later Submit() an error. Atomic so Is_shut_down() needs no lock.
        std::atomic<bool> shutdown_called{ false };

        // Serializes Shutdown() callers: the first one does the work, every
        // other caller blocks here until it has finished, so a returning
        // Shutdown() always means "the workers are joined".
        std::mutex shutdown_mutex;
        bool       shutdown_complete = false;     // guarded by shutdown_mutex

        // ---- Submission gate ---------------------------------------

        // Every submission registers here for as long as it is enqueueing,
        // and Shutdown() waits for the registered submissions before it
        // closes the queue. The result: a group is enqueued completely or
        // not at all, never half-way (see Submission_scope).
        std::mutex              gate_mutex;
        std::condition_variable gate_cv;
        size_t                  active_submissions = 0;   // guarded by gate_mutex

        // Number of tasks whose callable threw while running on any thread
        // of this dispatcher (workers and stealing callers). Such failures
        // are logged and counted here so the application can detect them;
        // see Run_task().
        std::atomic<size_t> failed_task_count{ 0 };

        // RAII registration in the submission gate. Throws std::runtime_error
        // from its constructor if Shutdown() has already started. While an
        // object is alive, Shutdown() will not close the queue, so every
        // Enqueue_or_throw() made under it can only fail for lack of room -
        // which it solves by running tasks, not by blocking or throwing.
        // Copying is disabled; nesting (a stolen task submitting while its
        // thread is already inside a scope) is fine, the gate is a counter.
        class Submission_scope
        {
            Thread_Dispatcher& dispatcher;

        public:

            explicit Submission_scope(Thread_Dispatcher& _dispatcher)
                : dispatcher(_dispatcher)
            {
                dispatcher.Begin_submission();
            }

            ~Submission_scope()
            {
                dispatcher.End_submission();
            }

            Submission_scope(const Submission_scope&) = delete;
            Submission_scope& operator=(const Submission_scope&) = delete;
        };

    public:

        // =========================================================
        // Lifecycle
        // =========================================================

        // Constructs the dispatcher and spawns worker threads.
        // _thread_count == 0  ->  hardware_concurrency() - 1
        //   (reserves the main thread; minimum 1 worker).
        // _queue_capacity: maximum number of tasks queued at once. When the
        //   queue is full a submitting thread does not sleep: it runs queued
        //   tasks itself until there is room (see Submit()).
        //
        // If a thread cannot be created (std::system_error), the workers
        // already started are stopped and joined before the exception is
        // rethrown: a constructor failure never leaves running threads
        // behind and never calls std::terminate.
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

        // Enqueues a single task.
        // The task must already carry its Counter_Ptr (if any).
        // Task is not copyable; caller must std::move it in.
        //
        // If the queue is full the calling thread executes queued tasks
        // until a slot frees up instead of blocking. That is what makes
        // submitting from inside a task (a nested Parallel_For, a
        // continuation) safe: even if every worker submits at once, the
        // queue keeps draining because nobody sleeps on it.
        //
        // Throws std::invalid_argument for an empty task (one that was
        // default-constructed or already moved from), or for a task whose
        // counter is already at zero (its Decrement() would underflow, or
        // its waiters would be released before the task ran), in every build.
        // Throws std::runtime_error after Shutdown(): a task accepted then
        // would never run and its counter would never reach zero.
        void Submit(Task _task);

        // Enqueues a contiguous span of tasks.
        // All tasks must already share the same Counter_Ptr (option A);
        // the whole span is validated before any task is moved into the
        // queue, and std::invalid_argument is thrown if a task is empty,
        // carries a different counter than the first one, or the shared
        // counter holds fewer pending tasks than the span has (it would
        // reach zero while part of the group is still running).
        // Returns the Counter_Ptr from the first task so the caller can
        // wait on it or chain continuations; returns nullptr if the
        // span is empty or the tasks have no counter.
        //
        // All-or-nothing with respect to Shutdown(): either the whole span
        // is enqueued or none of it is (std::runtime_error). A concurrent
        // Shutdown() waits for a submission in progress, so a group is never
        // left half-queued with a counter that cannot reach zero.
        // Same full-queue behavior as Submit().
        Counter_Ptr Submit_group(std::span<Task> _tasks);

        // =========================================================
        // Submit + work-stealing wait
        // =========================================================

        // Overload A: caller already has a Task with a Counter_Ptr.
        // Throws std::invalid_argument if the task is empty, carries no
        // counter (without one there is nothing to wait for) or its counter
        // is already at zero.
        // Steals work from the queue while waiting; does not sleep.
        // Intended for the main thread; avoid calling from worker threads
        // unless the work is nested (that is supported, see Parallel_For).
        //
        // Failures: if any task of THIS group threw, on whichever thread,
        // the wait runs to completion and the first such exception is
        // rethrown afterwards. Exceptions of tasks that belong to other
        // groups, which this thread may have stolen while waiting, are never
        // rethrown here: they are logged and counted (see Run_task).
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
        // group is done; steals work while waiting (same as above), and
        // rethrows the first failure of the group once it is done.
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

        // Number of tasks that threw while running on a worker thread or on
        // a thread that was stealing work, since the dispatcher was created.
        // Non-zero means at least one failure was logged to stderr. The
        // failure of a task that belongs to a group is also delivered to the
        // group's waiter (Atomic_Counter::Rethrow_if_failed); this count and
        // the log cover every task, including the fire-and-forget ones that
        // nobody waits for.
        size_t Failed_task_count() const;

        // =========================================================
        // Shutdown
        // =========================================================

        // Stops accepting tasks, lets the workers DRAIN every task already
        // queued (so every counter those tasks carry reaches zero), then
        // joins all workers. Safe to call multiple times and from any
        // thread except a worker (std::logic_error: a worker cannot join
        // itself). Do not call it from inside a task either: a task stolen
        // by a waiting thread runs on that thread, which may itself be in
        // the middle of a submission that Shutdown() would wait for.
        //
        // Concurrent calls are serialized: every caller returns only after
        // the workers have been joined, not just the first one, so "Shutdown()
        // returned" always means it is safe to release what the tasks use.
        // Submissions that are already enqueueing are allowed to finish first
        // (never interrupted half-way); later ones throw std::runtime_error.
        // Must be called before destroying resources that queued tasks
        // may still reference (EngineCore controls the order).
        void Shutdown();

    private:

        // =========================================================
        // Internal helpers
        // =========================================================

        // Entry point for every worker thread.
        // Loops on queue.Pop() -> Run_task() until the queue reports
        // it is finished (closed and drained), at which point Pop()
        // returns nullopt and the loop exits.
        void Worker_loop();

        // Executes a task on the calling thread and never throws. An
        // exception escaping a std::thread terminates the whole process, and
        // an exception escaping a stolen task would abort the wait of a
        // thread that does not own that task, so every failure is contained
        // here: logged to stderr and counted in failed_task_count. The task
        // already delivered it to its own group (Task::Execute) and released
        // the group's counter, so no waiter is left blocked by the failure.
        void Run_task(Task& _task) noexcept;

        // Pops one task without blocking and runs it (Run_task). Returns
        // false if the queue had none.
        bool Try_run_one() noexcept;

        // Logs and counts the exception currently held in _error.
        void Report_task_failure(std::exception_ptr _error) noexcept;

        // True if the calling thread is one of this dispatcher's workers.
        bool Is_worker_thread() const noexcept;

        // Steals and executes tasks from the queue until _counter is done.
        // Yields while the queue is empty and other workers are still
        // running tasks. Returns early only if the queue was cancelled,
        // since a cancelled queue drops tasks and the counter might then
        // never reach zero (std::runtime_error in that case).
        // A stolen task that throws does not abort the wait, and its
        // exception is NOT rethrown here - it may belong to another group
        // (see Run_task). The wait only surfaces the failure recorded in
        // _counter itself, i.e. the failure of the group being waited on,
        // and only once the group is done: returning earlier would let the
        // caller release data that tasks still running on the workers
        // reference.
        // Used by Submit_and_wait / Submit_group_and_wait to keep the
        // calling thread productive while its submitted tasks finish.
        void Steal_until_done(const Counter_Ptr& _counter);

        // Registration in the submission gate; see Submission_scope.
        void Begin_submission();
        void End_submission() noexcept;

        // Enqueues one task. Precondition: the calling thread holds a
        // Submission_scope, so the queue cannot be closed under it. When the
        // queue is full it runs queued tasks until a slot is free.
        // Throws std::runtime_error only if the queue no longer accepts
        // tasks, which cannot happen while a Submission_scope is held.
        void Enqueue_or_throw(Task&& _task);
    };

} // namespace ThreadDispatcher
