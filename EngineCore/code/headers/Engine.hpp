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
#include <Material_Component.hpp>
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
        // Registers _material in the Renderer's material table if it has no
        // slot yet, stores the slot in _material.gpu_material_id and returns
        // it. Texture handles are resolved to bindless indices here, once:
        // an unassigned albedo uses Default_Texture::White, and an assigned
        // one without a GPU index (never uploaded, or a stale handle) uses
        // Default_Texture::Error, so the mistake shows up magenta. Equal
        // materials share one slot (Renderer::Register_material).
        uint32_t Ensure_material_registered(ECS::Material_Component& _material);
        // Creates an entity with a transform and a mesh at _position, with
        // the mesh uploaded if needed.
        ECS::Entity Spawn_mesh_entity(CoreTypes::Asset_Handle _mesh, const MathLib::Vector3& _position);

        // Creates the initial test scene: camera, a textured sphere, a cube
        // and a directional light. Temporary until the editor exists.
        void Setup_scene();

        // Adds the stress content of the GPU-driven roadmap to the scene: a
        // floor, a grid of thousands of opaque and transparent objects
        // (some non-uniformly scaled) and hundreds of point lights. Every
        // value is deterministic, so the scene is the same on every run and
        // the old and new render paths can be compared on it. Also adds the
        // two validation setups below.
        void Setup_test_scene();

        // Validation of the order-independent transparency: transparent
        // objects of strongly different colors that overlap on screen, and
        // the cases no per-object sort resolves: an object inside another,
        // two objects that intersect, and a large object next to a small
        // one whose center is closer to the camera while part of the large
        // one is in front of it. Floats above the reference objects, in
        // front of the grid.
        void Setup_transparency_test();

        // Validation of the bounding volume culling under shear: a parent
        // without a mesh scaled (2, 1, 1) and a child cube rotated 45
        // degrees about Z. Their product has shear: the longest column of
        // its 3x3 underestimates the extent along X (1.58 instead of 2).
        // With show_bounds the ellipsoid must enclose the cube; with
        // freeze_culling, the cube must stay drawn while partly visible at
        // the edge of the frozen frustum.
        void Setup_shear_test();
    };

} // namespace EngineCore
