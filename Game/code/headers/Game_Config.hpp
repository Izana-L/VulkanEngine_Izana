#pragma once

#include <Engine_Config.hpp>

namespace Game
{

    // Root folder of the game's assets, relative to the working directory
    // the game is started from (the Visual Studio project directory,
    // Game/projects, so ../../Game/assets is <repository>/Game/assets).
    inline constexpr const char* ASSETS_ROOT = "../../Game/assets";

    // Builds the configuration the engine starts with. Everything that
    // belongs to the game and not to the engine is decided here: the
    // window, the input bindings and where the assets are.
    EngineCore::Engine_Config Make_engine_config();

} // namespace Game