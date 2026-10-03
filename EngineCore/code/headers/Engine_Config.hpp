#pragma once

#include <Validation_Mode.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace EngineCore
{

    // Engine_Config: everything the host application decides before the
    // engine starts. Plain data grouped by subsystem: a new subsystem adds a
    // section with default values and the Engine constructor does not change.
    //
    // The defaults are neutral: they name no game and no directory of the
    // repository. The application (Game/main.cpp) fills in what is its own.

    struct Window_Config
    {
        uint32_t    width = 1280;
        uint32_t    height = 720;
        std::string title = "Vulkan Engine";
    };

    struct Input_Config
    {
        // JSON with the action bindings. The application must set it.
        std::string bindings_path;
    };

    struct Paths_Config
    {
        // Root folder of the application's assets. Asset paths are built
        // from it, so they no longer depend on the working directory
        // layout hardcoded inside the engine.
        std::string assets_root;
    };

    struct Render_Config
    {
        // Overrides the engine's default validation level (see
        // Select_validation_mode in Engine.cpp). Empty = use the default.
        std::optional<Renderer_System::Validation_Mode> validation;
    };

    struct Engine_Config
    {
        Window_Config window;
        Input_Config  input;
        Paths_Config  paths;
        Render_Config render;
    };

} // namespace EngineCore