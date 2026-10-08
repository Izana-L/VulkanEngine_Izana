#include <Engine_Loop.hpp>

#include <Engine_Context.hpp>
#include <Window.hpp>
#include <Input.hpp>
#include <Renderer.hpp>
#include <Resource_Manager.hpp>
#include <World.hpp>
#include <Transform_System.hpp>
#include <Extractor.hpp>
#include <RenderPacket.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>

namespace EngineCore
{

    namespace
    {
        // Failures of the same Renderer call in a row (a frame, or the
        // recreation of the swapchain) after which the loop gives up. A
        // failure of the Renderer leaves it able to draw the next frame, so
        // an isolated one (for example, a moment without memory) is only
        // logged; a Renderer that fails again and again is not going to
        // recover, and looping on it would only print the same error forever.
        constexpr uint32_t MAX_CONSECUTIVE_RENDERER_FAILURES = 5;

        // Longest an iteration with nothing to draw waits for an event (no
        // active camera, or no surface to build a swapchain for). Nothing is
        // presented then, so nothing else paces the loop, which would spin
        // a core at 100 %. The wait ends at once when an event arrives; the
        // systems keep running at this rate meanwhile, so a camera created
        // by one of them is picked up.
        constexpr double IDLE_FRAME_PERIOD = 1.0 / 60.0;
    }

    void Engine_Loop::Run(Platform::Window& _window,
        Renderer_System::Renderer& _renderer,
        ResourceManager::Resource_Manager& _resources,
        Extractor& _extractor,
        Engine_Context& _context)
    {
        Input_System::Input& input = _context.Input();
        ECS::World& world = _context.World();

        Renderer_System::RenderPacket packet;

        Extract_Params extract_params;
        extract_params.opaque_pipeline_id = _renderer.Get_opaque_pipeline_id();
        extract_params.transparent_pipeline_id = _renderer.Get_transparent_pipeline_id();

        // Failures of each Renderer call since the last time that same call
        // returned normally. One counter per call, each cleared by its own
        // success: the frames skipped for lack of a camera never clear a
        // streak of Render, and a recreation that works says nothing about
        // whether Render does.
        uint32_t recreation_failures = 0;
        uint32_t render_failures = 0;

        // True while the "no active camera" warning is the last thing said
        // about the camera, so it is printed when the camera is lost and not
        // on every iteration.
        bool camera_missing_reported = false;

        // Runs a call into the Renderer that can throw. A failure is logged
        // and counted in _failures, and reported to the caller as false; it
        // does not leave the loop by itself. A call that returns normally
        // clears _failures.
        const auto guarded = [](const char* _what, uint32_t& _failures, const auto& _call) -> bool
            {
                try
                {
                    _call();
                    _failures = 0;
                    return true;
                }
                catch (const std::exception& _error)
                {
                    ++_failures;

                    std::cerr << "[Engine_Loop] " << _what << " failed (" << _failures
                        << " in a row): " << _error.what() << "\n";

                    return false;
                }
            };

        // True, after logging why, when the loop cannot go on: the Renderer
        // is lost (no further call can succeed), or it keeps failing.
        const auto must_stop = [&]() -> bool
            {
                if (_renderer.Is_lost())
                {
                    std::cerr << "[Engine_Loop] The Renderer is lost: leaving the main loop.\n";
                    return true;
                }

                const uint32_t worst_streak = std::max(recreation_failures, render_failures);

                if (worst_streak >= MAX_CONSECUTIVE_RENDERER_FAILURES)
                {
                    std::cerr << "[Engine_Loop] " << worst_streak
                        << " consecutive Renderer failures: leaving the main loop.\n";
                    return true;
                }

                return false;
            };

        while (!_window.Should_close())
        {
            // ── 1. OS events -> GLFW callbacks ────────────────────
            _window.Poll_events();

            if (_window.Is_minimized())
            {
                // No frame is produced while minimized, so nothing consumes
                // what the callbacks accumulated: drop it, or the first
                // frame after restoring would receive the whole backlog as
                // one mouse delta. Time is deliberately NOT updated here:
                // the pause shows up as one long (clamped) delta on the
                // next real frame, and the clocks follow real time anyway.
                input.Discard_pending();
                _window.Wait_events();
                continue;
            }

            // ── 2. Timing ─────────────────────────────────────────
            time.Update();
            const float dt = time.Get_delta_time();

            // ── 3. Resize: window flag -> renderer ────────────────
            // The only place that recreates the swapchain, whatever asked
            // for it (a resize, or the driver reporting it out of date).
            // Applied before extract, so the aspect ratio below and the
            // viewport the frame is drawn with come from the same extent.
            // A recreation that fails stays pending and is tried again on
            // the next iteration.
            if (_window.Consume_resized_flag())
                _renderer.Notify_framebuffer_resized();

            bool swapchain_ready = false;

            if (!guarded("Swapchain recreation", recreation_failures,
                [&] { swapchain_ready = _renderer.Recreate_swapchain_if_needed(); }))
            {
                if (must_stop())
                    break;

                continue;
            }

            if (!swapchain_ready)
            {
                // The surface has no area to build a swapchain for (the
                // Renderer's view of a minimized window), so no frame can be
                // drawn. Like the minimized case above: what the callbacks
                // accumulated has no frame to go to. The wait has a timeout,
                // since the window itself did not report being minimized and
                // there may be no event coming.
                input.Discard_pending();
                _window.Wait_events_timeout(IDLE_FRAME_PERIOD);
                continue;
            }

            // ── 4. Input snapshots + named actions ────────────────
            input.Update();

            // Systems of the Input phase (the render debug switches today).
            _context.Run_phase(Phase::Input, dt);

            // ── 5. Gameplay ───────────────────────────────────────
            _context.Run_phase(Phase::Gameplay, dt);

            // ── 6. Simulation ─────────────────────────────────────
            // Fixed_Update is reserved for physics: it runs once per frame
            // for now, with no accumulator.
            _context.Run_phase(Phase::Fixed_Update, dt);
            _context.Run_phase(Phase::Simulation, dt);

            // The engine's own system of this phase, always the last one:
            // the matrices are recomputed after everything that moved an
            // entity this frame (TRS, hierarchy).
            _context.Transforms().Update(world);

            // ── 7. Extract ECS -> RenderPacket ────────────────────
            _context.Run_phase(Phase::Extract, dt);

            uint32_t render_width = 0;
            uint32_t render_height = 0;
            _renderer.Get_render_size(render_width, render_height);

            extract_params.aspect_ratio = (render_height > 0)
                ? static_cast<float>(render_width) / static_cast<float>(render_height)
                : 1.0f;

            const bool has_camera = _extractor.Extract(world, _resources, extract_params, packet);

            // ── 8. Render ─────────────────────────────────────────
            _context.Run_phase(Phase::Render, dt);

            if (!has_camera)
            {
                // Nothing is drawn, so nothing is presented and nothing
                // paces the loop: wait for events instead of spinning.
                if (!camera_missing_reported)
                {
                    std::cerr << "[Engine_Loop] No active camera: nothing is rendered until one exists.\n";
                    camera_missing_reported = true;
                }

                _window.Wait_events_timeout(IDLE_FRAME_PERIOD);
                continue;
            }

            if (camera_missing_reported)
            {
                std::cout << "[Engine_Loop] An active camera exists again: rendering resumes.\n";
                camera_missing_reported = false;
            }

            // A failed frame has been undone by the Renderer, so the loop
            // goes on with the next one unless the Renderer is lost or keeps
            // failing.
            if (!guarded("Render", render_failures, [&] { _renderer.Render(packet); }) && must_stop())
                break;
        }
    }

} // namespace EngineCore