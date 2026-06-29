#include <Engine.hpp>

#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        EngineCore::Engine engine;
        engine.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Fatal] " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}