#pragma once

#include <MathConstants.hpp>

#include <chrono>
#include <cstdint>

namespace EngineCore
{

    // Time: centralizes all timing data for a single frame.
    //
    // Owned by Engine_Loop, updated once per frame before any system runs.
    // Read by any system that needs delta time or total elapsed time.
    //
    // Usage:
    //   float dt      = time.delta_seconds;   // frame delta
    //   double elapsed = time.total_seconds;  // time since engine start
    //   float fps      = time.fps;            // smoothed FPS
    struct Time
    {
        // =========================================================
        // Per-frame data (updated every frame)
        // =========================================================

        // Time since the last frame in seconds.
        // Clamped to max_delta_seconds to avoid spiral-of-death when
        // the frame takes unusually long (e.g. debugger breakpoint).
        float delta_seconds;

        // Total time elapsed since the engine started, in seconds.
        double total_seconds;

        // Smoothed frames per second, averaged over FPS_SAMPLE_COUNT frames.
        float fps;

        // Total number of frames rendered since the engine started.
        uint64_t frame_count;

        // =========================================================
        // Configuration
        // =========================================================

        // Maximum allowed delta time in seconds.
        // Defaults to MathLib::Constants::MAX_DELTA_TIME (0.25s).
        float max_delta_seconds;

        // Number of recent frames used to smooth the FPS display.
        static constexpr uint32_t FPS_SAMPLE_COUNT = 60;

        // =========================================================
        // Constructor
        // =========================================================

        Time()
            : delta_seconds(0.0f)
            , total_seconds(0.0)
            , fps(0.0f)
            , frame_count(0)
            , max_delta_seconds(MathLib::Constants::MAX_DELTA_TIME)
            , fps_sample_index(0)
            , fps_sample_sum(0.0f)
            , fps_buffer_full(false)
        {
            // Zero-initialize the FPS sample buffer.
            for (float& s : fps_samples) s = 0.0f;
        }

        // =========================================================
        // Update — called once per frame by Engine_Loop
        // =========================================================

        void Update()
        {
            using Clock = std::chrono::steady_clock;
            using Duration = std::chrono::duration<double>;

            const auto now = Clock::now();

            if (frame_count == 0)
            {
                // First frame — initialize timestamps so delta is 0.
                start_time = now;
                last_time = now;
            }

            const double raw_delta = std::chrono::duration_cast<Duration>(now - last_time).count();

            // Clamp to avoid spiral-of-death on long pauses.
            delta_seconds = static_cast<float>(raw_delta < max_delta_seconds ? raw_delta : max_delta_seconds);

            total_seconds = std::chrono::duration_cast<Duration>(now - start_time).count();

            last_time = now;
            ++frame_count;

            Update_fps(delta_seconds);
        }

    private:

        // =========================================================
        // Internal state
        // =========================================================

        using Clock_Point = std::chrono::steady_clock::time_point;

        Clock_Point start_time;
        Clock_Point last_time;

        // Circular buffer of recent delta times for FPS smoothing.
        float    fps_samples[FPS_SAMPLE_COUNT];
        uint32_t fps_sample_index;
        float    fps_sample_sum;
        bool     fps_buffer_full;

        void Update_fps(float _delta)
        {
            // Subtract the oldest sample before overwriting it.
            fps_sample_sum -= fps_samples[fps_sample_index];
            fps_samples[fps_sample_index] = _delta;
            fps_sample_sum += _delta;

            fps_sample_index = (fps_sample_index + 1) % FPS_SAMPLE_COUNT;

            if (!fps_buffer_full && fps_sample_index == 0)
                fps_buffer_full = true;

            const uint32_t count = fps_buffer_full ? FPS_SAMPLE_COUNT : fps_sample_index;

            if (count > 0 && fps_sample_sum > MathLib::Constants::EPSILON)
                fps = static_cast<float>(count) / fps_sample_sum;
        }
    };

} // namespace EngineCore