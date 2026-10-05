#pragma once

#include <Application.hpp>
#include <Camera_Controller.hpp>
#include <Render_Debug_Controller.hpp>

namespace Game
{

    // Demo_Application: the game the engine runs. It is the composition
    // root of the gameplay side: it owns the systems, builds the scene and
    // tells the engine in which phase each system runs. The engine owns the
    // loop and the order of the phases; this class never changes them.
    class Demo_Application : public EngineCore::Application
    {
    public:

        void On_start(EngineCore::Engine_Context& _context) override;

    private:

        // The engine only keeps pointers to the systems it runs, so they
        // live here, for as long as the application does.
        Camera_Controller       camera_controller;
        Render_Debug_Controller debug_controller;
    };

} // namespace Game