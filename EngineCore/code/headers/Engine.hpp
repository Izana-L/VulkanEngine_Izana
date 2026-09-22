#pragma once

#include <Window.hpp>
#include <Input.hpp>
#include <Renderer.hpp>
#include <Resource_Manager.hpp>
#include <World.hpp>
#include <Transform_System.hpp>
#include <Camera_Controller.hpp>
#include <Extractor.hpp>
#include <Engine_Loop.hpp>
#include <Entity.hpp>
#include <Asset_Handle.hpp>

#include <cstdint>
#include <string>

namespace EngineCore
{

    // Engine: owns and initializes all engine subsystems in dependency order.
    //
    // Construction order (matches member declaration order: C++ guarantees
    // members are constructed in declaration order and destroyed in reverse):
    //
    //   Layer 0: window                 (foundation)
    //   Layer 1: input, resources       (services)
    //   Layer 2: renderer               (GPU subsystem)
    //   Layer 3: world, transform_system, camera_controller, extractor
    //   Layer 4: loop                   (orchestration)
    //
    // Usage:
    //   Engine engine;
    //   engine.Run();
    //
    // Not copyable or movable: owns the entire engine lifetime.
    class Engine
    {
    public:

        Engine();
        ~Engine() = default;

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;

        // Builds the initial scene and starts the main loop.
        // Blocks until the window is closed.
        void Run();

    private:

        // =========================================================
        // Subsystems: declaration order = construction order
        // =========================================================

        // Layer 0: Foundation
        Platform::Window                    window;

        // Layer 1: Services
        Input_System::Input                 input;
        ResourceManager::Resource_Manager   resources;

        // Layer 2: GPU
        Renderer_System::Renderer           renderer;

        // Layer 3: Simulation
        ECS::World                          world;
        Transform_System                    transform_system;
        Camera_Controller                   camera_controller;
        Extractor                           extractor;

        // Layer 4: Orchestration
        Engine_Loop                         loop;

        // =========================================================
        // Scene state
        // =========================================================

        // Entity that has Transform_Component + Camera_Component.
        // Created by Setup_scene(), passed to Engine_Loop::Run().
        ECS::Entity camera_entity = ECS::INVALID_ENTITY;

        // =========================================================
        // Internal helpers
        // =========================================================

        // Bridges ResourceManager (Layer 1, Vulkan-agnostic) and the
        // Renderer (Layer 2): uploads the asset to the GPU if it has no
        // gpu id yet and registers the id. A cache hit in the resource
        // manager therefore never uploads twice: the second caller finds
        // the id already registered.
        uint32_t Ensure_mesh_uploaded(CoreTypes::Asset_Handle _mesh);
        uint32_t Ensure_image_uploaded(CoreTypes::Asset_Handle _image);

        // Creates an entity with a transform and a mesh at _position, with
        // the mesh uploaded if needed.
        ECS::Entity Spawn_mesh_entity(CoreTypes::Asset_Handle _mesh, const MathLib::Vector3& _position);

        // Creates the initial test scene: camera, a textured sphere, a cube
        // and a directional light. Temporary until the editor exists.
        void Setup_scene();
    };

} // namespace EngineCore
