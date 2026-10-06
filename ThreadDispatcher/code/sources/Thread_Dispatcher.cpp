#include <Thread_Dispatcher.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace ThreadDispatcher
{

    namespace
    {
        // The dispatcher whose worker loop the current thread is running, or
        // nullptr for any other thread (main thread, user threads, a caller
        // that is merely stealing work). Written once by each worker at
        // start-up and read only by its own thread, so it needs no lock.
        thread_local const Thread_Dispatcher* current_worker_of = nullptr;

        // Validates a task about to be enqueued. Enforced in every build:
        // an empty task would only be detected inside Task::Execute() on a
        // worker thread, far from the call that produced it.
        void Require_valid(const Task& _task, const char* _where)
        {
            if (!_task.Is_valid())
            {
                throw std::invalid_argument(std::string(_where) +
                    ": the task is empty (default-constructed or already moved from)");
            }
        }

        // Validates that a counter can account for _needed tasks that are
        // about to be submitted. A counter at zero (or lower than the group)
        // is a construction mistake with two ugly outcomes: waiters released
        // before the tasks ran - so the caller returns, and its stack data
        // dies, while a worker still uses it - and a Decrement() that
        // underflows later on a worker thread, far from the cause.
        // Tasks without a counter are not tracked, so there is nothing to check.
        void Require_pending(const Counter_Ptr& _counter, size_t _needed, const char* _where)
        {
            if (!_counter) return;

            const uint32_t pending = _counter->Get_count();

            if (pending < _needed)
            {
                throw std::invalid_argument(std::string(_where) +
                    ": the counter has " + std::to_string(pending) +
                    " pending task(s) but " + std::to_string(_needed) +
                    " tracked task(s) are being submitted; it would reach zero too early "
                    "or underflow. Create it with the number of tasks, or Increment() it "
                    "before submitting");
            }
        }
    }

    // =========================================================
    // Lifecycle
    // =========================================================

    Thread_Dispatcher::Thread_Dispatcher(size_t _thread_count,
        size_t _queue_capacity)
        : queue(_queue_capacity)
    {
        // 0 -> use all hardware threads minus the main thread, minimum 1.
        if (_thread_count == 0)
        {
            const size_t hardware = std::thread::hardware_concurrency();
            _thread_count = hardware > 1 ? hardware - 1 : 1;
        }

        try
        {
            workers.reserve(_thread_count);

            for (size_t i = 0; i < _thread_count; ++i)
            {
                workers.emplace_back(&Thread_Dispatcher::Worker_loop, this);
            }
        }
        catch (...)
        {
            // Thread creation can fail (std::system_error: out of threads or
            // memory). The destructor does not run for an object whose
            // constructor threw, and destroying a joinable std::thread calls
            // std::terminate, so the workers that did start must be stopped
            // and joined here, before the members they use go away.
            Shutdown();
            throw;
        }
    }

    Thread_Dispatcher::~Thread_Dispatcher()
    {
        Shutdown();
    }

    // =========================================================
    // Submit
    // =========================================================

    void Thread_Dispatcher::Begin_submission()
    {
        std::lock_guard< std::mutex > lock(gate_mutex);

        if (shutdown_called.load(std::memory_order_relaxed))
        {
            throw std::runtime_error("Thread_Dispatcher: task submitted after Shutdown()");
        }

        ++active_submissions;
    }

    void Thread_Dispatcher::End_submission() noexcept
    {
        std::lock_guard< std::mutex > lock(gate_mutex);

        if (--active_submissions == 0 && shutdown_called.load(std::memory_order_relaxed))
        {
            gate_cv.notify_all();
        }
    }

    void Thread_Dispatcher::Enqueue_or_throw(Task&& _task)
    {
        while (true)
        {
            // Try_emplace only moves from _task when it stores it, so a Full
            // result leaves the task intact for the next attempt.
            switch (queue.Try_emplace(std::move(_task)))
            {
            case Circular_Queue< Task >::Try_push_result::Pushed:
                return;

            case Circular_Queue< Task >::Try_push_result::Closed:
                throw std::runtime_error("Thread_Dispatcher: task submitted after Shutdown()");

            case Circular_Queue< Task >::Try_push_result::Full:
                break;
            }

            // The queue is full. Sleeping here would be a deadlock when the
            // submitter is itself a worker (or a task running on a stealing
            // thread): with every worker asleep on a full queue, nobody is
            // left to drain it. Make room by doing the work instead.
            if (!Try_run_one())
            {
                // Somebody else took the task between the two calls.
                std::this_thread::yield();
            }
        }
    }

    void Thread_Dispatcher::Submit(Task _task)
    {
        Require_valid(_task, "Thread_Dispatcher::Submit");
        Require_pending(_task.Get_counter(), 1, "Thread_Dispatcher::Submit");

        Submission_scope scope(*this);
        Enqueue_or_throw(std::move(_task));
    }

    Counter_Ptr
        Thread_Dispatcher::Submit_group(std::span<Task> _tasks)
    {
        if (_tasks.empty()) return nullptr;

        // The whole span is validated before a single task is moved into
        // the queue: a moved task cannot be handed back, so failing half-way
        // through would leave the caller with a group that is partially
        // running and a counter that never reaches zero.
        const Counter_Ptr shared_counter = _tasks[0].Get_counter();

        for (const Task& task : _tasks)
        {
            Require_valid(task, "Thread_Dispatcher::Submit_group");

            if (task.Get_counter() != shared_counter)
            {
                throw std::invalid_argument(
                    "Thread_Dispatcher::Submit_group: every task of a group must share "
                    "the same Counter_Ptr");
            }
        }

        Require_pending(shared_counter, _tasks.size(), "Thread_Dispatcher::Submit_group");

        // Registered for the whole loop: Shutdown() cannot close the queue
        // in the middle of it, so from here on the group is enqueued in full.
        // (If it could, the tasks already queued would run while the rest
        // stayed with the caller, and the counter would never reach zero.)
        Submission_scope scope(*this);

        for (Task& task : _tasks)
        {
            Enqueue_or_throw(std::move(task));
        }

        return shared_counter;
    }

    // =========================================================
    // Submit + work-stealing wait
    // =========================================================

    void Thread_Dispatcher::Submit_and_wait(Task _task)
    {
        Require_valid(_task, "Thread_Dispatcher::Submit_and_wait");

        if (!_task.Has_counter())
        {
            throw std::invalid_argument(
                "Thread_Dispatcher::Submit_and_wait: the task carries no Counter_Ptr, so "
                "there is nothing to wait for. Use the callable overload, which creates one");
        }

        Counter_Ptr counter = _task.Get_counter();
        Require_pending(counter, 1, "Thread_Dispatcher::Submit_and_wait");

        // The scope covers the enqueue only. Shutdown() has to wait for
        // submissions, not for the work they started: holding it during the
        // wait would make Shutdown() wait for this group's completion.
        {
            Submission_scope scope(*this);
            Enqueue_or_throw(std::move(_task));
        }

        Steal_until_done(counter);
    }

    void Thread_Dispatcher::Submit_group_and_wait(
        std::span<Task> _tasks)
    {
        if (_tasks.empty()) return;

        // Checked before Submit_group() moves the tasks away. Submit_group()
        // then guarantees that every task carries this same counter.
        if (!_tasks[0].Has_counter())
        {
            throw std::invalid_argument(
                "Thread_Dispatcher::Submit_group_and_wait: the tasks carry no shared "
                "Counter_Ptr, so there is nothing to wait for");
        }

        Counter_Ptr shared_counter = Submit_group(_tasks);

        Steal_until_done(shared_counter);
    }

    // =========================================================
    // Query
    // =========================================================

    size_t Thread_Dispatcher::Thread_count() const
    {
        return workers.size();
    }

    size_t Thread_Dispatcher::Pending_count() const
    {
        return queue.Size();
    }

    bool Thread_Dispatcher::Is_shut_down() const
    {
        return shutdown_called.load(std::memory_order_acquire);
    }

    size_t Thread_Dispatcher::Failed_task_count() const
    {
        return failed_task_count.load(std::memory_order_relaxed);
    }

    // =========================================================
    // Shutdown
    // =========================================================

    bool Thread_Dispatcher::Is_worker_thread() const noexcept
    {
        return current_worker_of == this;
    }

    void Thread_Dispatcher::Shutdown()
    {
        // A worker joining the workers would join itself: std::thread::join()
        // reports that as an exception, and the wait for in-flight
        // submissions below could wait for the very thread that is calling.
        if (Is_worker_thread())
        {
            throw std::logic_error(
                "Thread_Dispatcher::Shutdown: cannot be called from a worker thread");
        }

        // Serializes every caller. The first one performs the shutdown while
        // holding the mutex; a concurrent caller blocks here and, once it
        // gets in, finds the work done. A bare atomic exchange would let the
        // second caller return while the first one is still joining.
        std::lock_guard< std::mutex > serialize(shutdown_mutex);

        if (shutdown_complete) return;

        {
            std::unique_lock< std::mutex > lock(gate_mutex);

            // From here on no new submission is admitted (Begin_submission)...
            shutdown_called.store(true, std::memory_order_release);

            // ...and those that were admitted finish enqueueing. The workers
            // are still running and the queue is still open, so they make
            // progress: this wait is bounded by the enqueue itself.
            gate_cv.wait(lock, [this] { return active_submissions == 0; });
        }

        // Close, not Cancel: the workers finish everything already queued
        // before their Pop() reports the queue as finished, so no task is
        // dropped and every Counter_Ptr they carry reaches zero.
        queue.Close();

        for (auto& worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        shutdown_complete = true;
    }

    // =========================================================
    // Internal helpers
    // =========================================================

    void Thread_Dispatcher::Report_task_failure(std::exception_ptr _error) noexcept
    {
        failed_task_count.fetch_add(1, std::memory_order_relaxed);

        // Logging is best effort: running out of memory while building the
        // message must not take the thread (and with it the process) down.
        try
        {
            std::string message = "[Thread_Dispatcher] task failed: ";

            try
            {
                if (_error)
                {
                    std::rethrow_exception(_error);
                }

                message += "unknown error";
            }
            catch (const std::exception& error)
            {
                message += error.what();
            }
            catch (...)
            {
                message += "non-standard exception";
            }

            message += '\n';

            // Written as one string so concurrent threads cannot interleave
            // fragments of their messages.
            std::cerr << message;
        }
        catch (...)
        {
        }
    }

    void Thread_Dispatcher::Run_task(Task& _task) noexcept
    {
        try
        {
            _task.Execute();
        }
        catch (...)
        {
            Report_task_failure(std::current_exception());
        }
    }

    bool Thread_Dispatcher::Try_run_one() noexcept
    {
        auto task = queue.Try_pop();

        if (!task.has_value()) return false;

        Run_task(*task);
        return true;
    }

    void Thread_Dispatcher::Worker_loop()
    {
        current_worker_of = this;

        while (true)
        {
            // Blocking pop: sleeps until a task arrives or the queue is
            // finished (closed and drained, or cancelled).
            auto task = queue.Pop();

            if (!task.has_value()) break;

            // A worker must outlive a failing task: Run_task() contains the
            // exception. Task::Execute() has already delivered it to the
            // task's group and released the counter, so no waiter is blocked
            // by the failure.
            Run_task(*task);
        }
    }

    void Thread_Dispatcher::Steal_until_done(
        const Counter_Ptr& _counter)
    {
        if (!_counter)
        {
            throw std::invalid_argument(
                "Thread_Dispatcher::Steal_until_done: the counter is null");
        }

        // Is_done() reads with acquire ordering, so the results written by
        // the tasks that decremented the counter (and by their callbacks)
        // are visible to the caller once this loop exits.
        while (!_counter->Is_done())
        {
            // Try to grab a task from the queue without blocking. The task
            // may belong to any group: whatever it throws is contained by
            // Run_task() instead of being rethrown at this caller, who would
            // otherwise receive - and lose its own wait to - a failure that
            // is none of its business.
            if (Try_run_one()) continue;

            // A cancelled queue has dropped its tasks: the counter may never
            // reach zero, so spinning here would never end.
            if (queue.Is_cancelled())
            {
                throw std::runtime_error(
                    "Thread_Dispatcher::Steal_until_done: the queue was cancelled before "
                    "the group finished");
            }

            // Queue is empty but counter is not zero yet: workers are still
            // processing tasks. Yield to let them run instead of spinning
            // a full core.
            std::this_thread::yield();
        }

        // Only now, with the group complete, hand over ITS failure (if any):
        // the first exception raised by one of its own tasks, whichever
        // thread ran it.
        _counter->Rethrow_if_failed();
    }

} // namespace ThreadDispatcher
