#pragma once

#include <Time.hpp>

namespace Platform { class Window; }
namespace Renderer_System { class Renderer; }
namespace ResourceManager { class Resource_Manager; }

namespace EngineCore
{

    class Engine_Context;
    class Extractor;

    // Engine_Loop: drives the main game loop.
    //
    // Owns the Time struct and iterates frames until the window requests
    // close. Each frame runs in a fixed, deterministic order, in which the
    // engine's own steps and the systems of the application (registered by
    // Phase in the Engine_Context) interleave:
    //
    //   1. Window::Poll_events()         - GLFW dispatches callbacks; Input
    //                                      accumulates the raw events
    //      (minimized: discard pending input, wait for events, restart)
    //   2. Time::Update()                - delta time, FPS
    //   3. Resize handling               - Window flag -> Renderer swapchain;
    //      the only place that recreates it
    //   4. Input::Update()               - publish edges and deltas, flush
    //                                      actions
    //      Phase::Input systems          - after the engine's input steps
    //   5. Phase::Gameplay systems
    //   6. Phase::Fixed_Update systems   - reserved for physics: once per
    //                                      frame for now, no accumulator yet
    //      Phase::Simulation systems
    //      Transform_System::Update()    - the engine's own system of this
    //                                      phase, always the last one
    //   7. Phase::Extract systems, then
    //      Extractor::Extract()          - ECS -> RenderPacket
    //   8. Phase::Render systems, then
    //      Renderer::Render()            - draw the frame
    //
    // The Renderer's calls in steps 3 and 8 may throw. A failure is logged
    // and the loop goes on with the next frame, since a failed frame is
    // undone by the Renderer; it leaves the loop when the Renderer reports
    // itself lost, or after several failures in a row.
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

        // Runs the loop until window.Should_close() returns true, or the
        // Renderer is lost or keeps failing.
        // _context gives the loop the world, the transform system and the
        // input, and holds the systems it runs in each phase.
        void Run(Platform::Window& _window,
            Renderer_System::Renderer& _renderer,
            ResourceManager::Resource_Manager& _resources,
            Extractor& _extractor,
            Engine_Context& _context);

        const Platform::Time& Get_time() const { return time; }

    private:

        // The engine's single clock. Also hosts the named profiler timers
        // (Start_timer / Scoped_timer) for instrumenting the running loop.
        Platform::Time time;
    };

} // namespace EngineCore