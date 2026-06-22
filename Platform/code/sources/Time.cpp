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

        std::chrono::duration<float> elapsed = now - last_frame_time;
        float raw_delta = elapsed.count();

        // Clamp to avoid huge spikes (e.g. after a breakpoint, window drag-resize
        // stall, or the very first frame) - prevents physics/gameplay from
        // taking a giant simulation step that could break things.
        constexpr float max_delta = 0.25f;
        raw_delta = std::min(raw_delta, max_delta);

        unscaled_delta_time = raw_delta;
        delta_time = raw_delta * time_scale;

        unscaled_total_time += static_cast<double>(unscaled_delta_time);
        total_time += static_cast<double>(delta_time);

        last_frame_time = now;
        ++frame_count;

        // Update rolling window for average FPS
        recent_delta_times.push_back(unscaled_delta_time);
        if (recent_delta_times.size() > max_recent_samples) {
            recent_delta_times.pop_front();
        }

        // Optional frame rate cap: if the frame finished faster than the
        // target, sleep for the remaining time.
        if (target_fps > 0.0f) {
            float target_frame_time = 1.0f / target_fps;
            float frame_elapsed = std::chrono::duration<float>(Clock::now() - now).count();
            float remaining = target_frame_time - raw_delta - frame_elapsed;

            if (remaining > 0.0f) {
                std::this_thread::sleep_for(std::chrono::duration<float>(remaining));
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

