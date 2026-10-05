#include <Game_Config.hpp>

#include <Filesystem.hpp>

namespace Game
{

    EngineCore::Engine_Config Make_engine_config()
    {
        EngineCore::Engine_Config config;

        config.window.width = 1280;
        config.window.height = 720;
        config.window.title = "Vulkan Engine";

        config.paths.assets_root = ASSETS_ROOT;
        config.input.bindings_path =
            Platform::Filesystem::Combine_path(ASSETS_ROOT, "input/default_input_actions.json");

        return config;
    }

} // namespace Game