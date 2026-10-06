#include <Time.hpp>
#include <thread>
#include <numeric>
#include <algorithm>
#ifdef _WIN32
// NOMINMAX: <windows.h> would otherwise define min/max macros that break
// std::min / std::max below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Platform {

    // =========================================================
    // Precise waiting (frame rate limiter)
    // =========================================================

    namespace {

        using Wait_clock = std::chrono::steady_clock;

        // How long before the deadline the thread stops sleeping and
        // busy-waits instead. A sleeping thread is woken up late by the
        // scheduler, by up to one timer tick; the margin has to cover that
        // lateness so the deadline is reached by spinning, not overshot.
        // std::this_thread::sleep_until alone has the full tick of error:
        // about 15.6 ms by default on Windows, i.e. a whole 60 FPS frame.
#ifdef _WIN32
        // The high-resolution timer below wakes up within about a millisecond.
        constexpr std::chrono::microseconds spin_margin{ 2000 };
#else
        constexpr std::chrono::microseconds spin_margin{ 1000 };
#endif

#ifdef _WIN32
        // A high-resolution waitable timer (Windows 10 1803 and later): its
        // wake-up error is around a millisecond instead of the system tick,
        // and unlike timeBeginPeriod it needs no change to the global timer
        // resolution. Creation fails on older systems, which is reported by
        // Sleep_for() returning false so the caller can fall back.
        class High_resolution_timer
        {
        public:
            High_resolution_timer()
                : handle(CreateWaitableTimerExW(nullptr, nullptr, high_resolution_flag, TIMER_ALL_ACCESS)) {}

            ~High_resolution_timer() {
                if (handle != nullptr) CloseHandle(handle);
            }

            High_resolution_timer(const High_resolution_timer&) = delete;
            High_resolution_timer& operator=(const High_resolution_timer&) = delete;

            // Blocks for _duration. Returns false if it could not wait
            // (no high-resolution timer, or the wait failed); true otherwise.
            bool Sleep_for(std::chrono::nanoseconds _duration) {
                if (handle == nullptr) return false;
                if (_duration.count() < 100) return true;

                // A negative due time is relative, in units of 100 ns.
                LARGE_INTEGER due_time;
                due_time.QuadPart = -static_cast<LONGLONG>(_duration.count() / 100);
                if (!SetWaitableTimer(handle, &due_time, 0, nullptr, nullptr, FALSE)) return false;

                // The timeout is a safety net only: a timer that never fires
                // must not hang the main loop.
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(_duration);
                const DWORD timeout = static_cast<DWORD>(milliseconds.count() + 100);
                return WaitForSingleObject(handle, timeout) == WAIT_OBJECT_0;
            }

        private:
#ifdef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
            static constexpr DWORD high_resolution_flag = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
#else
            static constexpr DWORD high_resolution_flag = 0x00000002;
#endif
            HANDLE handle;
        };
#endif

        // Sleeps until _target, as precisely as the platform's sleep allows.
        void Sleep_until_target(Wait_clock::time_point _target) {
#ifdef _WIN32
            // One timer per thread: a waitable timer can only be armed by
            // one wait at a time.
            thread_local High_resolution_timer timer;

            const auto remaining = _target - Wait_clock::now();
            if (remaining <= Wait_clock::duration::zero()) return;
            if (timer.Sleep_for(remaining)) return;
#endif
            std::this_thread::sleep_until(_target);
        }

        // Returns when _deadline has been reached, within microseconds:
        // sleep (cheaply) until shortly before it, then spin the rest.
        void Wait_until(Wait_clock::time_point _deadline) {
            Sleep_until_target(_deadline - spin_margin);

            while (Wait_clock::now() < _deadline) {
                std::this_thread::yield();
            }
        }
    }

    // =========================================================
    // Time
    // =========================================================

    Time::Time()
        : start_time(Clock::now()),
        last_frame_time(start_time),
        delta_time(0.0f),
        unscaled_delta_time(0.0f),
        real_delta_time(0.0),
        total_time(0.0),
        unscaled_total_time(0.0),
        time_scale(1.0f),
        target_fps(0.0f),
        frame_count(0),
        fixed_time_accumulator(0.0f) {}

    void Time::Update() 
    {
        Time_point now = Clock::now();

        // Frame rate cap. The deadline is measured from the PREVIOUS
        // Update(), which is the start of the previous frame: sleeping until
        // then paces every frame to the same period. Subtracting the
        // previous delta from the period would count the previous sleep
        // twice and make frames alternate between sleeping and not.
        if (target_fps > 0.0f) {
            const auto period = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / static_cast<double>(target_fps)));
            const Time_point deadline = last_frame_time + period;

            if (now < deadline) {
                Wait_until(deadline);
                now = Clock::now();
            }
        }

        const double raw_delta = std::chrono::duration<double>(now - last_frame_time).count();
        const bool is_first_update = (frame_count == 0);

        // Clamp the SIMULATION step to avoid huge spikes (e.g. after a
        // breakpoint, window drag-resize stall, or the very first frame) -
        // prevents physics/gameplay from taking a giant step.
        const double clamped_delta = std::min(raw_delta, static_cast<double>(max_delta));

        unscaled_delta_time = static_cast<float>(clamped_delta);
        delta_time = static_cast<float>(clamped_delta) * time_scale;

        // The clocks follow the real interval, not the clamped one: a stall
        // is not lost, it is simply not simulated as a single step.
        unscaled_total_time = std::chrono::duration<double>(now - start_time).count();
        total_time += raw_delta * static_cast<double>(time_scale);

        // Statistics see the real interval, not the clamped step: a stall
        // must show up in the worst frame and the FPS figures at its true
        // length, which is the whole point of measuring them.
        real_delta_time = raw_delta;

        last_frame_time = now;
        ++frame_count;

        // Update rolling window for average FPS. The first Update() measures
        // from construction, i.e. engine startup (device creation, asset
        // loading), which is not a frame and would sit in the window, and in
        // Get_worst_frame_time(), for the next 60 frames.
        if (!is_first_update) {
            recent_delta_times.push_back(static_cast<float>(real_delta_time));
            if (recent_delta_times.size() > max_recent_samples) {
                recent_delta_times.pop_front();
            }
        }
    }

    // =========================================================
    // Delta time
    // =========================================================

    float Time::Get_delta_time() const {
        return delta_time;
    }

    float Time::Get_unscaled_delta_time() const {
        return unscaled_delta_time;
    }

    double Time::Get_delta_time_double() const {
        return static_cast<double>(delta_time);
    }

    // =========================================================
    // Total elapsed time
    // =========================================================

    float Time::Get_total_time() const {
        return static_cast<float>(total_time);
    }

    float Time::Get_unscaled_total_time() const {
        return static_cast<float>(unscaled_total_time);
    }

    double Time::Get_total_time_double() const {
        return total_time;
    }

    // =========================================================
    // Time scale
    // =========================================================

    void Time::Set_time_scale(float _scale) {
        time_scale = std::max(_scale, 0.0f);
    }

    float Time::Get_time_scale() const {
        return time_scale;
    }

    void Time::Pause() {
        Set_time_scale(0.0f);
    }

    void Time::Resume() {
        Set_time_scale(1.0f);
    }

    bool Time::Is_paused() const {
        return time_scale <= 0.0f;
    }

    // =========================================================
    // FPS and frame statistics
    // =========================================================

    float Time::Get_fps() const {
        if (real_delta_time <= 0.0) return 0.0f;
        return static_cast<float>(1.0 / real_delta_time);
    }

    float Time::Get_average_delta() const {
        if (recent_delta_times.empty()) return 0.0f;

        // Summed in double: 60 float samples would lose precision for nothing.
        const double sum = std::accumulate(recent_delta_times.begin(), recent_delta_times.end(), 0.0);
        return static_cast<float>(sum / static_cast<double>(recent_delta_times.size()));
    }

    float Time::Get_average_fps() const {
        const float average_delta = Get_average_delta();

        if (average_delta <= 0.0f) return 0.0f;
        return 1.0f / average_delta;
    }

    uint64_t Time::Get_frame_count() const {
        return frame_count;
    }


    // =========================================================
    // Frame spike detection
    // =========================================================

    float Time::Get_worst_frame_time() const {
        if (recent_delta_times.empty()) return 0.0f;
        return *std::max_element(recent_delta_times.begin(), recent_delta_times.end());
    }

    bool Time::Is_frame_spike(float _spike_multiplier) const {
        if (recent_delta_times.size() < 2) return false;

        const float average = Get_average_delta();

        if (average <= 0.0f) return false;
        return static_cast<float>(real_delta_time) > average * _spike_multiplier;
    }

    // =========================================================
    // Basic profiling
    // =========================================================

    void Time::Start_timer(const std::string& _name) {
        active_timers[_name] = Clock::now();
    }

    std::optional<float> Time::Stop_timer(const std::string& _name) {
        // Read the clock before anything else so the lookup below is not
        // part of the measured time.
        const Time_point stop_time = Clock::now();

        auto it = active_timers.find(_name);
        if (it == active_timers.end()) {
            return std::nullopt; // Stop_timer called without a matching Start_timer
        }

        const std::chrono::duration<float> elapsed = stop_time - it->second;
        const float duration = elapsed.count();

        timer_durations[_name] = duration;
        active_timers.erase(it);

        return duration;
    }

    std::optional<float> Time::Get_timer_duration(const std::string& _name) const {
        auto it = timer_durations.find(_name);
        if (it == timer_durations.end()) return std::nullopt;
        return it->second;
    }

    const std::unordered_map<std::string, float>& Time::Get_all_timer_durations() const {
        return timer_durations;
    }

    // =========================================================
    // Frame rate limiting
    // =========================================================

    void Time::Set_target_fps(float _target_fps) {
        target_fps = std::max(_target_fps, 0.0f);
    }

    float Time::Get_target_fps() const {
        return target_fps;
    }

    // =========================================================
    // Fixed timestep support
    // =========================================================

    int Time::Consume_fixed_steps(float _fixed_delta, int _max_steps) {
        if (_fixed_delta <= 0.0f) return 0;

        fixed_time_accumulator += delta_time;

        int steps = 0;
        while (fixed_time_accumulator >= _fixed_delta && steps < _max_steps) {
            fixed_time_accumulator -= _fixed_delta;
            ++steps;
        }

        // If we hit max_steps, drop the remaining accumulated time instead
        // of letting it pile up further - avoids the "spiral of death"
        // where the simulation can never catch up after a long stall.
        if (steps >= _max_steps) {
            fixed_time_accumulator = 0.0f;
        }

        return steps;
    }

    float Time::Get_fixed_alpha(float _fixed_delta) const {
        if (_fixed_delta <= 0.0f) return 0.0f;
        return fixed_time_accumulator / _fixed_delta;
    }

    // =========================================================
    // Scoped_timer
    // =========================================================

    Scoped_timer::Scoped_timer(Time& _time, const std::string& _name)
        : time(_time), name(_name) {
        time.Start_timer(name);
    }

    Scoped_timer::~Scoped_timer() {
        time.Stop_timer(name);
    }

}

