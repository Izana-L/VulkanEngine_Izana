#pragma once

#include <Time.hpp>
#include <Entity.hpp>

namespace Platform { class Window; }
namespace Input_System { class Input; }
namespace Renderer_System { class Renderer; }
namespace ResourceManager { class Resource_Manager; }
namespace ECS { class World; }

namespace EngineCore
{
    class Transform_System;
    class Camera_Controller;
    class Extractor;

    // Engine_Loop: drives the main game loop.
    //
    // Owns the Time struct and iterates frames until the window requests
    // close. Each frame runs the systems in a fixed, deterministic order:
    //
    //   1. Input::Begin_frame()          - snapshot previous input state
    //   2. Window::Poll_events()         - GLFW dispatches callbacks
    //      (minimized: discard pending input, wait for events, restart)
    //   3. Time::Update()                - delta time, FPS
    //   4. Resize handling               - Window flag -> Renderer swapchain
    //   5. Input::Update()               - publish deltas, flush actions
    //   6. Camera_Controller::Update()   - input -> camera transform
    //   7. Transform_System::Update()    - recompute TRS matrices
    //   8. Extractor::Extract()          - ECS -> RenderPacket
    //   9. Renderer::Render()            - draw the frame
    //
    // Separated from Engine so the loop strategy can be changed
    // (fixed timestep, render thread) without touching Engine's
    // construction/destruction logic.
    class Engine_Loop
    {
    public:

        Engine_Loop() = default;
        ~Engine_Loop() = default;

        Engine_Loop(const Engine_Loop&) = delete;
        Engine_Loop& operator=(const Engine_Loop&) = delete;

        // Runs the loop until window.Should_close() returns true.
        // _camera_entity: the ECS entity with Transform + Camera_Component
        //   that Camera_Controller will drive.
        void Run(Platform::Window& _window,
            Input_System::Input& _input,
            Renderer_System::Renderer& _renderer,
            ResourceManager::Resource_Manager& _resources,
            ECS::World& _world,
            Transform_System& _transform_system,
            Camera_Controller& _camera_controller,
            Extractor& _extractor,
            ECS::Entity                        _camera_entity);

        const Platform::Time& Get_time() const { return time; }

    private:

        // The engine's single clock. Also hosts the named profiler timers
        // (Start_timer / Scoped_timer) for instrumenting the running loop.
        Platform::Time time;
    };

} // namespace EngineCore
