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

        workers.reserve(_thread_count);

        for (size_t i = 0; i < _thread_count; ++i)
        {
            workers.emplace_back(&Thread_Dispatcher::Worker_loop, this);
        }
    }

    Thread_Dispatcher::~Thread_Dispatcher()
    {
        Shutdown();
    }

    // =========================================================
    // Submit
    // =========================================================

    void Thread_Dispatcher::Enqueue_or_throw(Task&& _task)
    {
        // Checked before AND reported after: Emplace() returns false when
        // the queue was closed while this call was blocked on a full queue.
        if (shutdown_called.load(std::memory_order_acquire) || !queue.Emplace(std::move(_task)))
        {
            throw std::runtime_error("Thread_Dispatcher: task submitted after Shutdown()");
        }
    }

    void Thread_Dispatcher::Submit(Task _task)
    {
        Require_valid(_task, "Thread_Dispatcher::Submit");
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
        Enqueue_or_throw(std::move(_task));
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

    void Thread_Dispatcher::Shutdown()
    {
        // exchange() makes the call idempotent even when two threads race
        // to shut down: exactly one of them proceeds past this line.
        if (shutdown_called.exchange(true, std::memory_order_acq_rel)) return;

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
    }

    // =========================================================
    // Internal helpers
    // =========================================================

    void Thread_Dispatcher::Worker_loop()
    {
        while (true)
        {
            // Blocking pop: sleeps until a task arrives or the queue is
            // finished (closed and drained, or cancelled).
            auto task = queue.Pop();

            if (!task.has_value()) break;

            // A worker must outlive a failing task: an exception escaping a
            // std::thread terminates the whole process. Task::Execute() has
            // already released the task's counter, so no waiter is blocked
            // by the failure; it is logged and counted instead.
            try
            {
                task->Execute();
            }
            catch (const std::exception& error)
            {
                failed_task_count.fetch_add(1, std::memory_order_relaxed);

                // Built as one string so concurrent workers cannot interleave
                // fragments of their messages.
                const std::string message =
                    std::string("[Thread_Dispatcher] task failed: ") + error.what() + '\n';
                std::cerr << message;
            }
            catch (...)
            {
                failed_task_count.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[Thread_Dispatcher] task failed with a non-standard exception\n";
            }
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

        // A stolen task that throws must not abort the wait: the rest of
        // the group is still running on the workers, and returning early
        // would let the caller release data those tasks still reference.
        // The first exception is kept and rethrown once the group is done.
        std::exception_ptr first_error;

        // Is_done() reads with acquire ordering, so the results written by
        // the tasks that decremented the counter are visible to the caller
        // once this loop exits.
        while (!_counter->Is_done())
        {
            // Try to grab a task from the queue without blocking.
            auto task = queue.Try_pop();

            if (task.has_value())
            {
                try
                {
                    task->Execute();
                }
                catch (...)
                {
                    if (!first_error)
                    {
                        first_error = std::current_exception();
                    }
                }

                continue;
            }

            // A cancelled queue has dropped its tasks: the counter may never
            // reach zero, so spinning here would never end.
            if (queue.Is_cancelled()) break;

            // Queue is empty but counter is not zero yet: workers are still
            // processing tasks. Yield to let them run instead of spinning
            // a full core.
            std::this_thread::yield();
        }

        if (first_error)
        {
            std::rethrow_exception(first_error);
        }
    }

} // namespace ThreadDispatcher
