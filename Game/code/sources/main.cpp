#include <Engine.hpp>

#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        EngineCore::Engine_Config config;
        config.input.bindings_path = "../../Input/jsons/default_input_actions.json";
        config.paths.assets_root = "../../Game/assets";

        EngineCore::Engine engine(config);
        engine.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Fatal] " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}