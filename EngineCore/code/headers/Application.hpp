#pragma once

namespace EngineCore
{

    class Engine_Context;

    // Application: what the host program gives the engine. The engine owns
    // the loop; the application builds the scene and registers its systems.
    class Application
    {
    public:

        virtual ~Application() = default;

        // Called once, after the engine is fully built (window, input with
        // its actions already loaded, renderer) and before the first frame.
        // Resolve action ids here, never earlier: Input::Load_actions
        // invalidates every id resolved before it.
        virtual void On_start(Engine_Context& _context) = 0;

        // Called once, after the loop ends and before the engine is
        // destroyed. Does nothing by default.
        virtual void On_shutdown(Engine_Context&) {}
    };

} // namespace EngineCore