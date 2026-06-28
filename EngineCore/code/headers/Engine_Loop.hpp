#pragma once

#include <Time.hpp>
#include <Id.hpp>

namespace Platform { class Window; }
namespace Input { class Input; }
namespace Renderer { class Renderer; }
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
    //   1. Time::Update()               — delta time, FPS
    //   2. Window::Poll_events()        — GLFW dispatches callbacks
    //   3. Input::Update()              — swap key snapshots, flush actions
    //   4. Camera_Controller::Update()  — input → camera transform
    //   5. Transform_System::Update()   — recompute TRS matrices
    //   6. Extractor::Extract()         — ECS → RenderPacket
    //   7. Renderer::Render()           — draw the frame
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
        //   that Camera_Controller and Extractor will use.
        void Run(Platform::Window& _window,
            Input::Input& _input,
            Renderer::Renderer& _renderer,
            ResourceManager::Resource_Manager& _resources,
            ECS::World& _world,
            Transform_System& _transform_system,
            Camera_Controller& _camera_controller,
            Extractor& _extractor,
            CoreTypes::Id                            _camera_entity);

        // Read-only access to timing data (for debug overlays, etc.)
        const Time& Get_time() const { return time; }

    private:

        Time time;
    };

} // namespace EngineCore