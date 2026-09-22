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

namespace EngineCore
{

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
