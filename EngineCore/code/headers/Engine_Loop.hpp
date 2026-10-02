#pragma once

#include <Time.hpp>
#include <Entity.hpp>

#include <cstddef>

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
    //   4. Resize handling               - Window flag -> Renderer swapchain;
    //      the only place that recreates it
    //   5. Input::Update()               - publish deltas, flush actions
    //      Debug switches                - input -> Renderer debug settings
    //   6. Camera_Controller::Update()   - input -> camera transform
    //   7. Transform_System::Update()    - recompute TRS matrices
    //   8. Extractor::Extract()          - ECS -> RenderPacket
    //   9. Renderer::Render()            - draw the frame
    //
    // The Renderer's calls in steps 4 and 9 may throw. A failure is logged
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

        // Resolves the debug actions listed at Handle_debug_input() to ids,
        // once. Call it after Input::Load_actions() (which invalidates every
        // cached id) and before Run(). A name missing from the input JSON is
        // reported on std::cerr and its switch never fires.
        void Bind_actions(const Input_System::Input& _input);

    private:

        // Applies the debug actions pressed this frame to the Renderer's
        // runtime switches (Renderer_System::Render_Debug_Settings):
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
        // Actions missing from the input JSON never fire (Bind_actions()
        // reports each one at startup).
        void Handle_debug_input(const Input_System::Input& _input, Renderer_System::Renderer& _renderer);

        // The engine's single clock. Also hosts the named profiler timers
        // (Start_timer / Scoped_timer) for instrumenting the running loop.
        Platform::Time time;

        // Action ids of the debug switches, set by Bind_actions().
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
    };

} // namespace EngineCore
