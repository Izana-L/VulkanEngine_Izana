#include <Demo_Application.hpp>

#include <Game_Config.hpp>
#include <Demo_Scene.hpp>

#include <Engine_Context.hpp>

namespace Game
{

    void Demo_Application::On_start(EngineCore::Engine_Context& _context)
    {
        // The action ids are resolved here and not earlier: the engine has
        // already loaded the input bindings, and Input::Load_actions
        // invalidates every id resolved before it.
        camera_controller.Bind_actions(_context.Input());
        debug_controller.Bind_actions(_context.Input());

        // The camera first: it must stay the first entity of the world, as
        // it always was, so the entity ids (and with them the iteration
        // order of the storages) do not change.
        camera_controller.Create_camera(_context.World(), { 0.0f, 1.0f, 5.0f });

        Demo_Scene scene(_context, ASSETS_ROOT);
        scene.Build();

        // Registration order is the execution order inside a phase.
        _context.Add_system(EngineCore::Phase::Input, debug_controller);
        _context.Add_system(EngineCore::Phase::Gameplay, camera_controller);
    }

} // namespace Game