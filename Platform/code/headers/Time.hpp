#pragma once


#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>


namespace Platform 
{
   
    // Time: tracks frame timing for the engine's main loop.
    // Owns a single steady clock and derives delta time, total elapsed time,
    // FPS, frame count, and supports time scaling (pause/slow-motion/fast-forward).
    //
    // Usage pattern: call Update() once per frame, as the very first thing
    // in the main loop, then read Get_delta_time() etc. for that frame.
    class Time 
    {
    private:

        using Clock = std::chrono::steady_clock;
        using Time_point = std::chrono::steady_clock::time_point;

        Time_point start_time;
        Time_point last_frame_time;

        float delta_time;
        float unscaled_delta_time;
        double total_time;
        double unscaled_total_time;

        float time_scale;
        float target_fps;

        uint64_t frame_count;

        std::deque<float> recent_delta_times;
        static constexpr size_t max_recent_samples = 60;

        float fixed_time_accumulator;

        // Profiling: start points for currently running named timers,
        // and the last recorded duration for each timer name.
        std::unordered_map<std::string, Time_point> active_timers;
        std::unordered_map<std::string, float> timer_durations;



    public:
        Time();

        // Recalculates delta time, total time, FPS and frame count.
        // Must be called exactly once per frame, at the start of the loop,
        // before any system reads delta time for that frame.
        void Update();

        // =========================================================
        // Delta time
        // =========================================================

        // Time elapsed since the previous frame, in seconds, multiplied
        // by the current time_scale. Use this for gameplay logic that
        // should respect pause/slow-motion (movement, AI, physics...).
        float Get_delta_time() const;

        // Time elapsed since the previous frame, in seconds, NOT affected
        // by time_scale. Use this for things that must keep running at
        // real speed even when the game is paused (UI animations, menu
        // transitions, debug overlays).
        float Get_unscaled_delta_time() const;

        // Same as Get_delta_time() but as a double, for cases needing
        // more precision (e.g. accumulating over a very long session).
        double Get_delta_time_double() const;

        // =========================================================
        // Total elapsed time
        // =========================================================

        // Total time elapsed since this Time instance was created,
        // affected by time_scale (a paused game stops advancing this too).
        float Get_total_time() const;

        // Total real-world time elapsed since this Time instance was
        // created, NOT affected by time_scale.
        float Get_unscaled_total_time() const;

        double Get_total_time_double() const;

        // =========================================================
        // Time scale (pause / slow-motion / fast-forward)
        // =========================================================

        // 1.0 = normal speed, 0.0 = paused, 0.5 = half speed (slow-motion),
        // 2.0 = double speed (fast-forward). Negative values are clamped to 0.
        void Set_time_scale(float _scale);
        float Get_time_scale() const;

        // Convenience helpers built on top of Set_time_scale
        void Pause();
        void Resume();
        bool Is_paused() const;

        // =========================================================
        // FPS and frame statistics
        // =========================================================

        // Instantaneous FPS, computed from the current frame's delta time.
        // Can fluctuate a lot frame to frame - prefer Get_average_fps()
        // for a stable value to display to the user.
        float Get_fps() const;

        // FPS averaged over a rolling window of recent frames (smoother,
        // more readable value for UI/debug display than the raw Get_fps()).
        float Get_average_fps() const;

        // Total number of frames processed since this Time instance started
        uint64_t Get_frame_count() const;

        // =========================================================
        // Frame rate limiting
        // =========================================================

        // Sets a target FPS cap (0 = uncapped). When set, Update() will
        // sleep at the end of the frame if it finished early, preventing
        // the loop from running faster than necessary (saves CPU/GPU power,
        // avoids unnecessarily high input polling rates on uncapped loops).
        void Set_target_fps(float _target_fps);
        float Get_target_fps() const;

        // =========================================================
        // Fixed timestep support (for deterministic physics)
        // =========================================================

        // Returns how many fixed-timestep physics steps should run this
        // frame, given an accumulator pattern. Call this once per frame;
        // it both returns the step count and consumes time from the
        // internal accumulator. fixed_delta is typically 1/60 or 1/50.
        // Clamps to max_steps to avoid a "spiral of death" after a long
        // stall (e.g. breakpoint, asset load hitch).
        int Consume_fixed_steps(float _fixed_delta, int _max_steps = 5);

        // The interpolation factor (0 to 1) between the last and next
        // fixed step, useful for smoothing rendering between physics
        // steps when the fixed timestep doesn't align with the render rate.
        float Get_fixed_alpha(float _fixed_delta) const;


        // =========================================================
        // Frame spike detection
        // =========================================================

        // Returns the longest unscaled delta time within the recent rolling
        // window (the "worst frame" recently). Useful to detect stutters
        // even when the average FPS looks healthy.
        float Get_worst_frame_time() const;

        // Returns true if the current frame's unscaled delta time exceeds
        // the average by more than the given multiplier (default 2x).
        // Useful for logging/flagging spikes as they happen.
        bool Is_frame_spike(float _spike_multiplier = 2.0f) const;

        // =========================================================
        // Basic profiling (named section timers)
        // =========================================================

        // Marks the start of a named timed section. Call Stop_timer with
        // the same name to record its duration. Prefer using Scoped_timer
        // below instead of calling these manually where possible.
        void Start_timer(const std::string& _name);

        // Stops a named timer and stores its duration, retrievable later
        // with Get_timer_duration(). Returns the duration in seconds.
        float Stop_timer(const std::string& _name);

        // Returns the duration (in seconds) recorded the last time the
        // named timer was stopped. Returns 0 if the timer was never used.
        float Get_timer_duration(const std::string& _name) const;

        // Returns all recorded timer names and their last duration -
        // useful for building a simple on-screen profiler overlay.
        const std::unordered_map<std::string, float>& Get_all_timer_durations() const;
    
    };
    class Scoped_timer
    {
    public:
        Scoped_timer(Time& _time, const std::string& _name);
        ~Scoped_timer();

        Scoped_timer(const Scoped_timer&) = delete;
        Scoped_timer& operator=(const Scoped_timer&) = delete;

    private:
        Time& time;
        std::string name;
    };

}