#include <Engine.hpp>
#include <Demo_Application.hpp>
#include <Game_Config.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        EngineCore::Engine engine(Game::Make_engine_config());

        // Declared after the engine: it is destroyed first.
        Game::Demo_Application application;

        engine.Run(application);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Fatal] " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}