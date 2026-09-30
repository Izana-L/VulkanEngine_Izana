#include <Time.hpp>
#include <thread>
#include <numeric>
#include <algorithm>

namespace Platform {

    Time::Time()
        : start_time(Clock::now()),
        last_frame_time(start_time),
        delta_time(0.0f),
        unscaled_delta_time(0.0f),
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
                std::this_thread::sleep_until(deadline);
                now = Clock::now();
            }
        }

        const double raw_delta = std::chrono::duration<double>(now - last_frame_time).count();

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

        last_frame_time = now;
        ++frame_count;

        // Update rolling window for average FPS
        recent_delta_times.push_back(unscaled_delta_time);
        if (recent_delta_times.size() > max_recent_samples) {
            recent_delta_times.pop_front();
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
        if (unscaled_delta_time <= 0.0f) return 0.0f;
        return 1.0f / unscaled_delta_time;
    }

    float Time::Get_average_fps() const {
        if (recent_delta_times.empty()) return 0.0f;

        float sum = std::accumulate(recent_delta_times.begin(), recent_delta_times.end(), 0.0f);
        float average_delta = sum / static_cast<float>(recent_delta_times.size());

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

        float sum = std::accumulate(recent_delta_times.begin(), recent_delta_times.end(), 0.0f);
        float average = sum / static_cast<float>(recent_delta_times.size());

        if (average <= 0.0f) return false;
        return unscaled_delta_time > average * _spike_multiplier;
    }

    // =========================================================
    // Basic profiling
    // =========================================================

    void Time::Start_timer(const std::string& _name) {
        active_timers[_name] = Clock::now();
    }

    float Time::Stop_timer(const std::string& _name) {
        auto it = active_timers.find(_name);
        if (it == active_timers.end()) {
            return 0.0f; // Stop_timer called without a matching Start_timer
        }

        std::chrono::duration<float> elapsed = Clock::now() - it->second;
        float duration = elapsed.count();

        timer_durations[_name] = duration;
        active_timers.erase(it);

        return duration;
    }

    float Time::Get_timer_duration(const std::string& _name) const {
        auto it = timer_durations.find(_name);
        if (it == timer_durations.end()) return 0.0f;
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

