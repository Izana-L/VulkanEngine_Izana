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

#include <cstdint>
#include <exception>
#include <iostream>

namespace EngineCore
{

    namespace
    {
        // Failures of the Renderer in a row (a frame, or the recreation of the
        // swapchain) after which the loop gives up. A failure of the Renderer
        // leaves it able to draw the next frame, so an isolated one (for
        // example, a moment without memory) is only logged; a Renderer that
        // fails again and again is not going to recover, and looping on it
        // would only print the same error forever.
        constexpr uint32_t MAX_CONSECUTIVE_RENDERER_FAILURES = 5;
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

        // Failures of the Renderer since its last successful frame.
        uint32_t consecutive_failures = 0;

        // Runs a call into the Renderer that can throw. A failure is logged
        // and counted, and reported to the caller as false; it does not leave
        // the loop by itself.
        const auto guarded = [&](const char* _what, const auto& _call) -> bool
            {
                try
                {
                    _call();
                    return true;
                }
                catch (const std::exception& _error)
                {
                    ++consecutive_failures;

                    std::cerr << "[Engine_Loop] " << _what << " failed (" << consecutive_failures
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

                if (consecutive_failures >= MAX_CONSECUTIVE_RENDERER_FAILURES)
                {
                    std::cerr << "[Engine_Loop] " << consecutive_failures
                        << " consecutive Renderer failures: leaving the main loop.\n";
                    return true;
                }

                return false;
            };

        while (!_window.Should_close())
        {
            // ── 1. Snapshot the input state of the previous frame ──
            input.Begin_frame();

            // ── 2. OS events -> GLFW callbacks ────────────────────
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

            // ── 3. Timing ─────────────────────────────────────────
            time.Update();
            const float dt = time.Get_delta_time();

            // ── 4. Resize: window flag -> renderer ────────────────
            // The only place that recreates the swapchain, whatever asked
            // for it (a resize, or the driver reporting it out of date).
            // Applied before extract, so the aspect ratio below and the
            // viewport the frame is drawn with come from the same extent.
            // A recreation that fails stays pending and is tried again on
            // the next iteration.
            if (_window.Consume_resized_flag())
                _renderer.Notify_framebuffer_resized();

            if (!guarded("Swapchain recreation", [&] { _renderer.Recreate_swapchain_if_needed(); }))
            {
                if (must_stop())
                    break;

                continue;
            }

            // ── 5. Input snapshots + named actions ────────────────
            input.Update();

            // Systems of the Input phase (the render debug switches today).
            _context.Run_phase(Phase::Input, dt);

            // ── 6. Gameplay ───────────────────────────────────────
            _context.Run_phase(Phase::Gameplay, dt);

            // ── 7. Simulation ─────────────────────────────────────
            // Fixed_Update is reserved for physics: it runs once per frame
            // for now, with no accumulator.
            _context.Run_phase(Phase::Fixed_Update, dt);
            _context.Run_phase(Phase::Simulation, dt);

            // The engine's own system of this phase, always the last one:
            // the matrices are recomputed after everything that moved an
            // entity this frame (TRS, hierarchy).
            _context.Transforms().Update(world);

            // ── 8. Extract ECS -> RenderPacket ────────────────────
            _context.Run_phase(Phase::Extract, dt);

            uint32_t render_width = 0;
            uint32_t render_height = 0;
            _renderer.Get_render_size(render_width, render_height);

            extract_params.aspect_ratio = (render_height > 0)
                ? static_cast<float>(render_width) / static_cast<float>(render_height)
                : 1.0f;

            const bool has_camera = _extractor.Extract(world, _resources, extract_params, packet);

            // ── 9. Render ─────────────────────────────────────────
            _context.Run_phase(Phase::Render, dt);

            // A failed frame has been undone by the Renderer, so the loop
            // goes on with the next one unless the Renderer is lost or keeps
            // failing.
            if (has_camera)
            {
                if (guarded("Render", [&] { _renderer.Render(packet); }))
                    consecutive_failures = 0;
                else if (must_stop())
                    break;
            }
        }
    }

} // namespace EngineCore