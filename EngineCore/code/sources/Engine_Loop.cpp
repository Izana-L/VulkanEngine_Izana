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
        Input::Input& _input,
        Renderer::Renderer& _renderer,
        ResourceManager::Resource_Manager& _resources,
        ECS::World& _world,
        Transform_System& _transform_system,
        Camera_Controller& _camera_controller,
        Extractor& _extractor,
        CoreTypes::Id                      _camera_entity)
    {
        CoreTypes::RenderPacket packet;

        while (!_window.Should_close())
        {
            // ── 1. Timing ─────────────────────────────────────────
            time.Update();
            const float dt = time.delta_seconds;

            // ── 2. OS events → GLFW callbacks ─────────────────────
            _window.Poll_events();

            // ── 3. Input snapshots + named actions ────────────────
            _input.Update();

            // ── 4. Camera (input → transform) ─────────────────────
            _camera_controller.Update(_camera_entity, _input, _world, dt);

            // ── 5. Transform matrices (TRS, hierarchy) ────────────
            _transform_system.Update(_world);

            // ── 6. Extract ECS → RenderPacket ─────────────────────
            int fb_width = 0;
            int fb_height = 0;
            _window.Get_framebuffer_size(fb_width, fb_height);

            const float aspect_ratio = (fb_height > 0)
                ? static_cast<float>(fb_width) / static_cast<float>(fb_height)
                : 1.0f;

            const bool has_camera =
                _extractor.Extract(_world, _resources, aspect_ratio, packet);

            // ── 7. Render ─────────────────────────────────────────
            if (has_camera)
                _renderer.Render(packet);
        }
    }

} // namespace EngineCore