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

namespace EngineCore
{

    namespace
    {
        // Next value of a cyclic enumeration with a Count enumerator.
        template <typename ENUM>
        ENUM Next_value(ENUM _value)
        {
            const uint32_t next = static_cast<uint32_t>(_value) + 1u;
            return static_cast<ENUM>(next % static_cast<uint32_t>(ENUM::Count));
        }
    }

    void Engine_Loop::Handle_debug_input(const Input_System::Input& _input, Renderer_System::Renderer& _renderer)
    {
        if (!debug_actions_resolved)
        {
            debug_action_ids.light_culling = _input.Get_action_id("DebugLightCulling");
            debug_action_ids.cluster_view = _input.Get_action_id("DebugClusterView");
            debug_action_ids.opaque_path = _input.Get_action_id("DebugOpaquePath");
            debug_action_ids.freeze_culling = _input.Get_action_id("DebugFreezeCulling");
            debug_action_ids.show_bounds = _input.Get_action_id("DebugShowBounds");
            debug_action_ids.stats = _input.Get_action_id("DebugStats");
            debug_actions_resolved = true;
        }

        Renderer_System::Render_Debug_Settings settings = _renderer.Get_debug_settings();
        bool changed = false;

        if (_input.Was_action_pressed(debug_action_ids.light_culling))
        {
            settings.light_culling = (settings.light_culling == Renderer_System::Light_Culling_Mode::Clustered)
                ? Renderer_System::Light_Culling_Mode::Brute_Force
                : Renderer_System::Light_Culling_Mode::Clustered;
            changed = true;
        }

        if (_input.Was_action_pressed(debug_action_ids.cluster_view))
        {
            settings.cluster_view = Next_value(settings.cluster_view);
            changed = true;
        }

        if (_input.Was_action_pressed(debug_action_ids.opaque_path))
        {
            settings.opaque_path = Next_value(settings.opaque_path);
            changed = true;
        }

        if (_input.Was_action_pressed(debug_action_ids.freeze_culling))
        {
            settings.freeze_culling = !settings.freeze_culling;
            changed = true;
        }

        if (_input.Was_action_pressed(debug_action_ids.show_bounds))
        {
            settings.show_bounds = !settings.show_bounds;
            changed = true;
        }

        if (_input.Was_action_pressed(debug_action_ids.stats))
        {
            settings.print_stats = !settings.print_stats;
            changed = true;
        }

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
            // Applied before extract, so the aspect ratio below and the
            // viewport the frame is drawn with come from the same extent.
            if (_window.Consume_resized_flag())
                _renderer.Notify_framebuffer_resized();

            _renderer.Recreate_swapchain_if_needed();

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
            if (has_camera)
                _renderer.Render(packet);
        }
    }

} // namespace EngineCore
