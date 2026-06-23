#include <Thread_Dispatcher.hpp>

#include <algorithm>
#include <cassert>
#include <thread>

namespace ThreadDispatcher
{

    // =========================================================
    // Lifecycle
    // =========================================================

    Thread_Dispatcher::Thread_Dispatcher(size_t _thread_count,
        size_t _queue_capacity)
        : queue(_queue_capacity)
    {
        // 0 → use all hardware threads minus the main thread, minimum 1.
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
        if (!shutdown_called)
        {
            Shutdown();
        }
    }

    // =========================================================
    // Submit
    // =========================================================

    void Thread_Dispatcher::Submit(Task _task)
    {
        assert(_task.Is_valid() && "Submit() called with an invalid Task");
        queue.Emplace(std::move(_task));
    }

    Counter_Ptr
        Thread_Dispatcher::Submit_group(std::span<Task> _tasks)
    {
        if (_tasks.empty()) return nullptr;

        // Grab the shared counter from the first task before moving tasks
        // into the queue — once moved, we can no longer read them.
       Counter_Ptr shared_counter = _tasks[0].Get_counter();

        for (auto& task : _tasks)
        {
            assert(task.Is_valid() && "Submit_group() contains an invalid Task");
            queue.Emplace(std::move(task));
        }

        return shared_counter;
    }

    // =========================================================
    // Submit + work-stealing wait
    // =========================================================

    void Thread_Dispatcher::Submit_and_wait(Task _task)
    {
        assert(_task.Is_valid() && "Submit_and_wait() called with an invalid Task");
        assert(_task.Has_counter() && "Submit_and_wait(Task) requires a Task with a "
            "Counter_Ptr. Use the callable overload instead: "
            "Submit_and_wait([&]{ ... })");

        Counter_Ptr counter = _task.Get_counter();
        queue.Emplace(std::move(_task));
        Steal_until_done(counter);
    }

    void Thread_Dispatcher::Submit_group_and_wait(
        std::span<Task> _tasks)
    {
        if (_tasks.empty()) return;

        Counter_Ptr shared_counter = Submit_group(_tasks);

        assert(shared_counter &&
            "Submit_group_and_wait() requires tasks with a shared Counter_Ptr.");

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

    // =========================================================
    // Shutdown
    // =========================================================

    void Thread_Dispatcher::Shutdown()
    {
        if (shutdown_called) return;
        shutdown_called = true;

        // Wake up all workers blocked on Pop() and make them exit.
        queue.Cancel();

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
            // Blocking pop: sleeps until a task arrives or Cancel() is called.
            auto task = queue.Pop();

            // nullopt means the queue was cancelled → shutdown, exit loop.
            if (!task.has_value()) break;

            task->Execute();
        }
    }

    void Thread_Dispatcher::Steal_until_done(
        const Counter_Ptr& _counter)
    {
        assert(_counter && "Steal_until_done() called with a null counter");

        while (!_counter->Is_done())
        {
            // Try to grab a task from the queue without blocking.
            auto task = queue.Try_pop();

            if (task.has_value())
            {
                task->Execute();
            }
            else
            {
                // Queue is empty but counter is not zero yet:
                // workers are still processing tasks. Yield to let
                // them run instead of spinning a full core.
                std::this_thread::yield();
            }
        }
    }

} // namespace JobSystem