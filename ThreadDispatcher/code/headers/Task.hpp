#pragma once

#include <Atomic_Counter.hpp>

#include <functional>
#include <stdexcept>
#include <utility>

namespace ThreadDispatcher
{

    // Task: the basic unit of work in the Job System. Wraps any callable
    // (lambda, functor, free function) together with an optional
    // Atomic_Counter to decrement when the work finishes.
    //
    // Type erasure happens exactly once, here, when the Task is constructed
    // from a template callable - after that, the Task is a concrete type
    // that can be stored in Circular_Queue<Task> without further indirection.
    //
    // The callable is stored as std::function<void()> because Task instances
    // must be homogeneous to live in the same queue. The overhead of
    // std::function (one virtual-call-like indirection per Execute()) is
    // acceptable here since it happens once per task, not per element in
    // a tight inner loop.
    struct Task
    {
    private:

        // The actual work to perform. std::function<void()> provides
        // type-erased storage for any callable - the type erasure cost
        // (one heap allocation for large callables, one indirect call
        // per Execute()) is paid once per task, which is acceptable.
        std::function< void() > callable;

        // Optional counter to decrement when Execute() completes.
        // nullptr means this is a fire-and-forget task with no tracking.
        // shared_ptr because the same counter may be shared with other
        // tasks (all tasks in a group share the same counter) and with
        // the thread that is Wait()-ing on the group.
        Counter_Ptr counter;

    public :

        // =========================================================
        // Construction
        // =========================================================

        // Default constructor: creates an empty, invalid task.
        // An empty task must not be executed - check Is_valid() first.
        Task() = default;

        // Constructs a task from any callable, with an optional counter
        // to decrement on completion.
        // _callable: any invocable with signature void() - lambda,
        //   functor, free function, std::bind result, etc.
        // _counter: if provided, Decrement() is called on it when
        //   Execute() finishes. Pass nullptr to skip counter tracking.
        template< typename CALLABLE >
        explicit Task(CALLABLE&& _callable, Counter_Ptr _counter = nullptr)
            : callable(std::forward< CALLABLE >(_callable)),
            counter(std::move(_counter))
        {
            // Verify at compile time that the callable can be invoked
            // with no arguments and returns void. This gives a clear
            // error message instead of a cryptic template failure.
            static_assert( std::is_invocable_r_v< void, CALLABLE >, "Task callable must be invocable as void()" );
        }

        // Tasks are movable (transferred into the queue) but not copyable
        // (std::function is copyable, but copying tasks with shared
        // counters could cause confusing double-decrements if both copies
        // are executed).
        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        Task(Task&&) = default;
        Task& operator=(Task&&) = default;

        // =========================================================
        // Execution
        // =========================================================

        // Executes the callable and, if a counter was provided, decrements
        // it to signal completion to waiting threads and to trigger chained
        // work registered through Set_on_zero.
        //
        // The counter is released even when the callable throws: a failed
        // task still counts as finished, otherwise every thread waiting on
        // its group would block forever. The exception then propagates to
        // the executing thread, which decides how to report it (see
        // Thread_Dispatcher::Worker_loop and Steal_until_done).
        //
        // A task can be executed once: the counter is detached before the
        // callable runs, so a second call can never decrement it again.
        // Throws std::logic_error if the task is empty.
        void Execute()
        {
            if (!Is_valid())
            {
                throw std::logic_error(
                    "Task::Execute: the task is empty (default-constructed or already moved from)");
            }

            // Detach the counter first so it is decremented exactly once no
            // matter how the callable exits.
            Counter_Ptr finished = std::move(counter);
            counter = nullptr;

            try
            {
                callable();
            }
            catch (...)
            {
                if (finished)
                {
                    finished->Decrement();
                }

                throw;
            }

            // Work first, then completion: waiters and on_zero continuations
            // must observe a fully completed task.
            if (finished)
            {
                finished->Decrement();
            }
        }

        // =========================================================
        // State queries
        // =========================================================

        // Returns true if this task has a callable to execute.
        // An invalid task is one that was default-constructed or moved from.
        bool Is_valid() const
        {
            return static_cast<bool>(callable);
        }

        // Returns true if this task has an associated counter that will
        // be decremented on completion.
        bool Has_counter() const
        {
            return counter != nullptr;
        }

        // Returns the counter associated with this task, or nullptr if none.
        // Useful for inspecting task dependencies without executing the task.
        const Counter_Ptr& Get_counter() const
        {
            return counter;
        }

    
    };

    // =========================================================
    // Factory functions
    // =========================================================

    // Creates a fire-and-forget Task from any callable.
    // No counter: no one will be notified when this task finishes.
    // Use when you just want to run something in the background
    // without waiting for the result.
    template< typename CALLABLE >
    Task Make_task(CALLABLE&& _callable)
    {
        return Task(std::forward< CALLABLE >(_callable));
    }

    // Creates a tracked Task from any callable and a shared counter.
    // The counter will be decremented when the task finishes, which
    // can wake up Wait() calls or trigger Set_on_zero callbacks.
    template< typename CALLABLE >
    Task Make_task(CALLABLE&& _callable, Counter_Ptr _counter)
    {
        return Task(std::forward< CALLABLE >(_callable), std::move(_counter));
    }

}