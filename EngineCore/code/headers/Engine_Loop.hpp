#pragma once

#include <Time.hpp>
#include <Entity.hpp>
#include <Alpha_Mode.hpp>

#include <cstddef>
#include <vector>

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
    // close. Before the first frame the clock is resynchronized
    // (Time::Resync), so the engine construction and the scene setup that
    // preceded Run() are not measured as the first frame's delta. Each
    // frame runs the systems in a fixed, deterministic order:
    //
    //   1. Window::Poll_events()         - GLFW dispatches callbacks; Input
    //                                      queues the key/button events
    //      (minimized: discard pending input, wait for events, restart)
    //   2. Time::Update()                - delta time, FPS
    //   3. Resize handling               - Window flag -> Renderer swapchain;
    //      the only place that recreates it
    //   4. Input::Update()               - publish deltas, replay the queued
    //                                      events into levels and actions
    //      Debug switches                - input -> Renderer debug settings
    //   5. Camera_Controller::Update()   - input -> camera transform
    //   6. Transform_System::Update()    - recompute TRS matrices
    //   7. Extractor::Extract()          - ECS -> RenderPacket
    //   8. Renderer::Render()            - draw the frame
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
        // _material_alpha_modes: alpha mode of every registered material
        //   slot (Extract_Params::material_alpha_modes). Read every frame,
        //   so materials registered while the loop runs are covered.
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
            const std::vector<CoreTypes::Alpha_Mode>& _material_alpha_modes,
            ECS::Entity                        _camera_entity);

        const Platform::Time& Get_time() const { return time; }

    private:

        // Applies the debug actions pressed this frame to the Renderer's
        // runtime switches (Renderer_System::Render_Debug_Settings). Every
        // press counts: a toggle pressed twice within one frame ends where
        // it started, and a cycle advances once per press.
        //   DebugLightCulling   - clustered lights <-> every light (reference)
        //   DebugClusterView    - cycles the cluster grid overlays
        //   DebugOpaquePath     - cycles direct / CPU indirect / GPU indirect /
        //                         GPU culled opaque draws
        //   DebugFreezeCulling  - freezes the culling camera
        //   DebugShowBounds     - bounding volume wireframes (the ellipsoids
        //                         the culling tests)
        //   DebugStats          - GPU timings and counters every second
        //   DebugIsolateTimings - isolated GPU timing scopes (a full barrier
        //                         before each; the totals are not frame times)
        // Actions missing from the input JSON simply never fire.
        void Handle_debug_input(const Input_System::Input& _input, Renderer_System::Renderer& _renderer);

        // The engine's single clock. Also hosts the named profiler timers
        // (Start_timer / Scoped_timer) for instrumenting the running loop.
        Platform::Time time;

        // Action ids of the debug switches, resolved on the first frame.
        struct Debug_Action_Ids
        {
            size_t light_culling = static_cast<size_t>(-1);
            size_t cluster_view = static_cast<size_t>(-1);
            size_t opaque_path = static_cast<size_t>(-1);
            size_t freeze_culling = static_cast<size_t>(-1);
            size_t show_bounds = static_cast<size_t>(-1);
            size_t stats = static_cast<size_t>(-1);
            size_t isolate_timings = static_cast<size_t>(-1);
        };

        Debug_Action_Ids debug_action_ids;
        bool             debug_actions_resolved = false;
    };

} // namespace EngineCore
