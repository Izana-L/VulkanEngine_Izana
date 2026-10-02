#include <Engine_Loop.hpp>

#include <Window.hpp>
#include <Input.hpp>
#include <Renderer.hpp>
#include <Resource_Manager.hpp>
#include <World.hpp>
#include <Transform_System.hpp>
#include <Camera_Controller.hpp>
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

        // Next value of a cyclic enumeration with a Count enumerator.
        template <typename ENUM>
        ENUM Next_value(ENUM _value)
        {
            const uint32_t next = static_cast<uint32_t>(_value) + 1u;
            return static_cast<ENUM>(next % static_cast<uint32_t>(ENUM::Count));
        }

        // Flips _value when _action was pressed this frame. True if it did.
        bool Toggle_on_press(const Input_System::Input& _input, size_t _action, bool& _value)
        {
            if (!_input.Was_action_pressed(_action))
                return false;

            _value = !_value;
            return true;
        }

        // Moves _value to its next enumerator when _action was pressed this
        // frame. True if it did.
        template <typename ENUM>
        bool Cycle_on_press(const Input_System::Input& _input, size_t _action, ENUM& _value)
        {
            if (!_input.Was_action_pressed(_action))
                return false;

            _value = Next_value(_value);
            return true;
        }
    }

    void Engine_Loop::Bind_actions(const Input_System::Input& _input)
    {
        constexpr const char* CONSUMER = "Engine_Loop";

        debug_action_ids.light_culling = _input.Resolve_action_id("DebugLightCulling", CONSUMER);
        debug_action_ids.cluster_view = _input.Resolve_action_id("DebugClusterView", CONSUMER);
        debug_action_ids.opaque_path = _input.Resolve_action_id("DebugOpaquePath", CONSUMER);
        debug_action_ids.freeze_culling = _input.Resolve_action_id("DebugFreezeCulling", CONSUMER);
        debug_action_ids.show_bounds = _input.Resolve_action_id("DebugShowBounds", CONSUMER);
        debug_action_ids.stats = _input.Resolve_action_id("DebugStats", CONSUMER);
        debug_action_ids.isolate_timings = _input.Resolve_action_id("DebugIsolateTimings", CONSUMER);
    }

    void Engine_Loop::Handle_debug_input(const Input_System::Input& _input, Renderer_System::Renderer& _renderer)
    {
        Renderer_System::Render_Debug_Settings settings = _renderer.Get_debug_settings();

        // |= and not ||: every switch is evaluated, since several can be
        // pressed in the same frame. Light_Culling_Mode has two values, so
        // cycling it is the switch between clustered and brute force.
        bool changed = false;

        changed |= Cycle_on_press(_input, debug_action_ids.light_culling, settings.light_culling);
        changed |= Cycle_on_press(_input, debug_action_ids.cluster_view, settings.cluster_view);
        changed |= Cycle_on_press(_input, debug_action_ids.opaque_path, settings.opaque_path);
        changed |= Toggle_on_press(_input, debug_action_ids.freeze_culling, settings.freeze_culling);
        changed |= Toggle_on_press(_input, debug_action_ids.show_bounds, settings.show_bounds);
        changed |= Toggle_on_press(_input, debug_action_ids.stats, settings.print_stats);
        changed |= Toggle_on_press(_input, debug_action_ids.isolate_timings, settings.isolate_gpu_timings);

        // The Renderer logs every value that changed.
        if (changed)
            _renderer.Set_debug_settings(settings);
    }

    void Engine_Loop::Run(Platform::Window& _window,
        Input_System::Input& _input,
        Renderer_System::Renderer& _renderer,
        ResourceManager::Resource_Manager& _resources,
        ECS::World& _world,
        Transform_System& _transform_system,
        Camera_Controller& _camera_controller,
        Extractor& _extractor,
        ECS::Entity                        _camera_entity)
    {
        CoreTypes::RenderPacket packet;

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
            _input.Begin_frame();

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
                _input.Discard_pending();
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
            _input.Update();

            // Runtime switches between the old and new render paths.
            Handle_debug_input(_input, _renderer);

            // ── 6. Camera (input -> transform) ────────────────────
            _camera_controller.Update(_camera_entity, _input, _world, dt);

            // ── 7. Transform matrices (TRS, hierarchy) ────────────
            _transform_system.Update(_world);

            // ── 8. Extract ECS -> RenderPacket ────────────────────
            uint32_t render_width = 0;
            uint32_t render_height = 0;
            _renderer.Get_render_size(render_width, render_height);

            extract_params.aspect_ratio = (render_height > 0)
                ? static_cast<float>(render_width) / static_cast<float>(render_height)
                : 1.0f;

            const bool has_camera = _extractor.Extract(_world, _resources, extract_params, packet);

            // ── 9. Render ─────────────────────────────────────────
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
