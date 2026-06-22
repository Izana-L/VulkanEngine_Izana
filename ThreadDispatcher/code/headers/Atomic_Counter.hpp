#pragma once

#include <atomic>
#include <cassert>
#include <chrono>      
#include <cstdint>
#include <functional>
#include <mutex>
#include <condition_variable>

namespace ThreadDispatcher
{

    // Atomic_Counter: a thread-safe reference-counted synchronization
    // primitive used to track the completion of a group of tasks.
 
    class Atomic_Counter
    {

    private:

        // The actual count. Uses atomic operations for lock-free
        // increment/decrement from multiple worker threads simultaneously.
        std::atomic< uint32_t > count;

        // Protects cv - needed because condition_variable requires a mutex.
        mutable std::mutex mutex;

        // Condition variable for Wait() - notified when count reaches zero.
        mutable std::condition_variable cv;

        // Optional callback invoked when count reaches zero.
        // Stored as std::function to accept any callable (lambda, functor...)
        std::function< void() > on_zero;
        std::function< void(uint32_t) >  on_decrement; 

    public:

        // Creates a counter with the given initial value.
        // Typically set to the number of tasks that will decrement it.
        explicit Atomic_Counter(uint32_t _initial_value = 0)
            : count(_initial_value)
        {}

        // Not copyable or movable - shared via shared_ptr, and the
        // address of the counter must remain stable since worker threads
        // hold raw pointers/references to it while decrementing.
        Atomic_Counter(const Atomic_Counter&) = delete;
        Atomic_Counter& operator=(const Atomic_Counter&) = delete;
        Atomic_Counter(Atomic_Counter&&) = delete;
        Atomic_Counter& operator=(Atomic_Counter&&) = delete;

        // =========================================================
        // Core operations
        // =========================================================

        // Increments the counter by _amount.
        // Call this before submitting additional tasks that should be
        // tracked by this counter, if you need to add tasks after
        // the initial construction.
        void Increment(uint32_t _amount = 1)
        {
            assert(_amount > 0 && "Increment() called with zero amount");
            count.fetch_add(_amount, std::memory_order_relaxed);
        }

        // Decrements the counter by 1. If it reaches zero, calls the
        // on_zero callback (if set) and notifies all threads waiting
        // in Wait(). Called automatically by Thread_Dispatcher when
        // a task associated with this counter finishes.
        void Decrement()
        {
            assert(count.load(std::memory_order_relaxed) > 0 &&
                "Decrement() called on a counter that is already zero");

            // memory_order_release: todas las escrituras de esta task
            // son visibles a los hilos que observen count==0 con acquire
            uint32_t previous = count.fetch_sub(1, std::memory_order_release);

            // Calculamos el valor RESTANTE tras el decremento para
            // pasarselo al callback on_decrement
            uint32_t remaining = previous - 1;

            // Llamamos a on_decrement ANTES de notificar a los waiters,
            // para que el progreso se reporte antes de que Wait() desbloquee
            // al hilo principal (coherente: primero actualiza el progreso,
            // luego desbloquea)
            if (on_decrement)
            {
                on_decrement(remaining);
            }

            if (previous == 1)
            {
                // El counter llegó a cero: despertamos a los waiters
                {
                    std::lock_guard< std::mutex > lock(mutex);
                    cv.notify_all();
                }

                // Llamamos a on_zero fuera del lock
                if (on_zero)
                {
                    on_zero();
                }
            }
        }

        // Returns the current value of the counter.
        uint32_t Get_count() const
        {
            return count.load(std::memory_order_relaxed);
        }

        // Returns true if the counter has reached zero.
        bool Is_done() const
        {
            return Get_count() == 0;
        }

        // =========================================================
        // Waiting
        // =========================================================

        // Blocks the calling thread until the counter reaches zero.
        // Typically called from the main thread or from a task that
        // depends on a group of other tasks finishing first.
        //
        // NOTE: avoid calling Wait() from inside a worker thread if
        // possible - it blocks that worker without doing useful work.
        // Prefer the on_zero callback pattern for task chaining instead.
        void Wait() const
        {
            // Acquire ordering: ensures all writes performed by tasks
            // before their Decrement() calls are visible to this thread
            // after Wait() returns.
            if (count.load(std::memory_order_acquire) == 0) return;

            std::unique_lock< std::mutex > lock(mutex);
            cv.wait(lock, [this]
                {
                    // Acquire inside the lambda ensures we see all writes
                    // from the tasks that decremented the counter.
                    return count.load(std::memory_order_acquire) == 0;
                });
        }
        // Comprueba si el contador ha llegado a cero SIN bloquear.
        // A diferencia de Wait() que duerme el hilo hasta que count==0,
        // Try_wait() devuelve inmediatamente con true o false.
        // Util para polling: comprobar periodicamente si un grupo de tasks
        // ha terminado mientras se hace otro trabajo en el hilo llamante.
        // memory_order_acquire: igual que en Wait(), garantiza que si
        // devuelve true, vemos todas las escrituras de las tasks que
        // decrementaron el counter antes de llegar a cero.
        bool Try_wait() const
        {
            return count.load(std::memory_order_acquire) == 0;
        }
        // Espera hasta que el contador llegue a cero O hasta que expire
        // el timeout dado. Devuelve true si el counter llegó a cero dentro
        // del tiempo limite, false si expiró el timeout.
        //
        // Util en Debug para detectar posibles deadlocks: si Wait_for()
        // devuelve false después de varios segundos, probablemente hay
        // una task que nunca termina o un counter que nunca se decrementa.
        //
        // También útil en sistemas con timeouts reales (carga de assets
        // de red, conexiones a servidores...) donde no puedes esperar
        // indefinidamente.
        bool Wait_for(std::chrono::milliseconds _timeout) const
        {
            // Comprobación rápida: si ya está a cero no hace falta esperar
            if (count.load(std::memory_order_acquire) == 0) return true;

            std::unique_lock< std::mutex > lock(mutex);

            // wait_for: igual que cv.wait() pero con un timeout maximo.
            // Devuelve std::cv_status::no_timeout si fue notificado antes
            // de que expirara el tiempo, o std::cv_status::timeout si
            // se agotó el tiempo sin ser notificado.
            // El predicado garantiza que no hay "spurious wakeups" (el
            // sistema puede despertar la condition_variable por razones
            // internas sin que nadie haya llamado notify - el predicado
            // verifica que el contador realmente llegó a cero).
            return cv.wait_for(lock, _timeout, [this]
            {
                return count.load(std::memory_order_acquire) == 0;
            });
        }
        // =========================================================
        // Callback (for task chaining without blocking)
        // =========================================================
        // 
        // Define un callback que se llama cada vez que el contador se
        // decrementa, recibiendo el valor RESTANTE después del decremento.
        // A diferencia de Set_on_zero (que solo se llama una vez al final),
        // este callback se invoca en CADA Decrement — útil para:
        // - Barras de progreso de carga de assets
        // - Logging granular de cuántas tasks han terminado
        // - Sistemas que necesitan reaccionar a cada paso, no solo al final
        //
        // El callback recibe el valor restante (cuántas tasks faltan aún):
        //   _callback(2) → faltan 2 tasks
        //   _callback(1) → falta 1 task
        //   _callback(0) → todas las tasks terminaron (equivalente a on_zero)
        //
        // NOTA: se ejecuta desde el worker thread que hizo el Decrement,
        // igual que on_zero. Mantenlo corto.
        // NOTA: si tienes tanto Set_on_decrement como Set_on_zero, ambos
        // se llamarán cuando el counter llegue a cero (primero on_decrement
        // con 0, luego on_zero).

        void Set_on_decrement(std::function< void(uint32_t) > _callback)
        {
            // No tiene sentido llamarlo dos veces en el mismo counter
            assert(!on_decrement && "Set_on_decrement() called twice on the same counter");
            on_decrement = std::move(_callback);
        }
        // Sets a callback to be called when the counter reaches zero.
        // Used to chain tasks: when all dependencies finish, the callback
        // submits the next task to the dispatcher automatically, without
        // any thread needing to block and wait.
        //
        // The callback is called from the worker thread that decrements
        // the counter to zero - keep it short (e.g. just submit a task).
        //
        // Must be set BEFORE the counter reaches zero to avoid a race.
        void Set_on_zero(std::function< void() > _callback)
        {
            assert(!on_zero && "Set_on_zero() called twice on the same counter");
            on_zero = std::move(_callback);
        }

        // Resets the counter to a new value, allowing reuse.
        // Must only be called when the counter is already at zero
        // and no threads are waiting or about to call Decrement().
        void Reset(uint32_t _new_value)
        {
            assert(Is_done() && "Reset() called on a counter that is not yet zero");

            // Limpiamos AMBOS callbacks del ciclo anterior
            on_zero = nullptr;
            on_decrement = nullptr; // ← nuevo

            count.store(_new_value, std::memory_order_relaxed);
        }

    
    };

    // Convenience alias - counters are always shared between the submitter
    // (who waits on it) and the workers (who decrement it), so shared_ptr
    // is the natural ownership model.
    using Counter_Ptr = std::shared_ptr< Atomic_Counter >;

    // Creates a shared Atomic_Counter with the given initial value.
    // Use this instead of make_shared<Atomic_Counter> directly for clarity.
    inline Counter_Ptr Make_counter(uint32_t _initial_value = 0)
    {
        return std::make_shared< Atomic_Counter >(_initial_value);
    }

}