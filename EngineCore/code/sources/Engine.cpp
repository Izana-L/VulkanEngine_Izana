#include <Engine.hpp>

#include <Application.hpp>
#include <Engine_Context.hpp>
#include <Environment.hpp>

#include <iostream>

namespace EngineCore
{
    namespace
    {
        // Validation level for the Renderer. It is the engine's default:
        // Engine_Config::render.validation overrides it.
        //   Release (NDEBUG) - Off. The layer adds CPU cost to every API
        //                      call and must not load in a shipped build,
        //                      even on a machine with the Vulkan SDK.
        //                      The ENGINE_GPU_AV variable is ignored.
        //                      Validation can still be forced from outside
        //                      the engine (Vulkan Configurator,
        //                      VK_INSTANCE_LAYERS) to investigate a
        //                      Release-only problem.
        //   Debug            - Standard, or Gpu_Assisted when the
        //                      ENGINE_GPU_AV variable equals "1". GPU-AV
        //                      slows every draw noticeably, so it is never
        //                      enabled without that explicit opt-in.
        Renderer_System::Validation_Mode Select_validation_mode()
        {
#ifdef NDEBUG
            return Renderer_System::Validation_Mode::Off;
#else
            // Environment variable that turns GPU-assisted validation on:
            // ENGINE_GPU_AV=1. Any other value, or its absence, keeps
            // standard validation.
            constexpr const char* GPU_AV_ENV_VAR = "ENGINE_GPU_AV";

            // An unset variable (std::nullopt) compares unequal to "1".
            const bool requested = Platform::Environment::Get_variable(GPU_AV_ENV_VAR) == "1";

            return requested ? Renderer_System::Validation_Mode::Gpu_Assisted : Renderer_System::Validation_Mode::Standard;
#endif
        }
    }

    // =========================================================
    // Constructor: builds subsystems in dependency order
    // =========================================================

    Engine::Engine(const Engine_Config& _config)
        : config(_config)

        // Layer 0: Foundation
        , window(config.window.width, config.window.height, config.window.title)

        // Layer 1: Services
        , input(window)
        , resources()

        // Layer 2: GPU
        , renderer(window, config.render.validation.value_or(Select_validation_mode()))   // Off in Release; Standard, or Gpu_Assisted with ENGINE_GPU_AV=1, in Debug; degrades if unavailable

        // Layer 2b: Bridge between the services and the GPU
        , assets(resources, renderer)

        // Layer 3: Simulation (no constructor args needed)
        , world()
        , transform_system()
        , extractor()

        // Layer 4: Orchestration
        , loop()
    {
        // Load action bindings from JSON. The application resolves its
        // action names in Application::On_start, which runs after this
        // constructor: Load_actions() rebuilds the action table, so an id
        // resolved before it would be invalid.
        input.Load_actions(config.input.bindings_path);

        std::cout << "[Engine] All subsystems initialized.\n";
    }

    // =========================================================
    // Run
    // =========================================================

    void Engine::Run(Application& _application)
    {
        Render_Debug   render_debug(renderer);
        Engine_Context context(world, transform_system, input, assets, loop.Get_time(), render_debug);

        _application.On_start(context);

        std::cout << "[Engine] Starting main loop.\n";

        loop.Run(window, renderer, resources, extractor, context);

        std::cout << "[Engine] Main loop ended.\n";

        _application.On_shutdown(context);
    }

} // namespace EngineCore