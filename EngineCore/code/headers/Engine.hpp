#pragma once

#include <Window.hpp>
#include <Input.hpp>
#include <Renderer.hpp>
#include <Resource_Manager.hpp>
#include <World.hpp>
#include <Transform_System.hpp>
#include <Extractor.hpp>
#include <Engine_Loop.hpp>
#include <Engine_Config.hpp>
#include <Gpu_Assets.hpp>

namespace EngineCore
{

    class Application;

    // Engine: owns and initializes all engine subsystems in dependency order.
    //
    // Construction order (matches member declaration order: C++ guarantees
    // members are constructed in declaration order and destroyed in reverse):
    //
    //   Config:  config                 (what the host decided)
    //   Layer 0: window                 (foundation)
    //   Layer 1: input, resources       (services)
    //   Layer 2: renderer               (GPU subsystem)
    //   Layer 2b: assets                (bridge: resources -> renderer)
    //   Layer 3: world, transform_system, extractor
    //   Layer 4: loop                   (orchestration)
    //
    // The engine knows nothing about the game: no scene, no controls, no
    // asset paths. The host program describes itself with an Engine_Config
    // and an Application, which builds the scene and registers its systems
    // in the phases of the loop.
    //
    // Usage:
    //   Engine_Config config;
    //   config.input.bindings_path = "...";
    //   config.paths.assets_root   = "...";
    //   Engine engine(config);
    //
    //   My_Application application;   // derives from Application
    //   engine.Run(application);
    //
    // Not copyable or movable: owns the entire engine lifetime.
    class Engine
    {
    public:

        explicit Engine(const Engine_Config& _config);
        ~Engine() = default;

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;

        // Calls _application.On_start(), runs the main loop until the window
        // is closed, then calls _application.On_shutdown().
        void Run(Application& _application);

    private:

        // =========================================================
        // Subsystems: declaration order = construction order
        // =========================================================

        // Configuration: first, the subsystems below are built from it.
        const Engine_Config                 config;

        // Layer 0: Foundation
        Platform::Window                    window;

        // Layer 1: Services
        Input_System::Input                 input;
        ResourceManager::Resource_Manager   resources;

        // Layer 2: GPU
        Renderer_System::Renderer           renderer;

        // Layer 2b: Bridge between the services and the GPU. Borrows
        // resources and renderer, so it comes after both.
        Gpu_Assets                          assets;

        // Layer 3: Simulation
        ECS::World                          world;
        ECS::Transform_System               transform_system;
        Extractor                           extractor;

        // Layer 4: Orchestration
        Engine_Loop                         loop;
    };

} // namespace EngineCore