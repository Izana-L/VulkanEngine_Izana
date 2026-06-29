#include <Engine.hpp>

#include <Transform_Component.hpp>
#include <Camera_Component.hpp>
#include <Mesh_Component.hpp>
#include <Light_Component.hpp>
#include <Primitive_Desc.hpp>
#include <MathConstants.hpp>

#include <glm/gtc/quaternion.hpp>

#include <iostream>

namespace EngineCore
{

    // =========================================================
    // Constructor — builds subsystems in dependency order
    // =========================================================

    Engine::Engine()
        // Layer 0 — Foundation
        : window(1280, 720, "Vulkan Engine")

        // Layer 1 — Services
        , input(window)
        , resources()

        // Layer 2 — GPU
        , renderer(window, true)   // true = enable validation layers

        // Layer 3 — Simulation (no constructor args needed)
        , world()
        , transform_system()
        , camera_controller()
        , extractor()

        // Layer 4 — Orchestration
        , loop()
    {
        // Load action bindings from JSON.
        input.Load_actions("../../Input/jsons/default_input_actions.json");

        std::cout << "[Engine] All subsystems initialized.\n";
    }

    // =========================================================
    // Run
    // =========================================================

    void Engine::Run()
    {
        Setup_scene();

        std::cout << "[Engine] Starting main loop.\n";

        loop.Run(window,
            input,
            renderer,
            resources,
            world,
            transform_system,
            camera_controller,
            extractor,
            camera_entity);

        std::cout << "[Engine] Main loop ended.\n";
    }

    // =========================================================
    // Setup_scene — test scene for Fase 1
    // =========================================================

    void Engine::Setup_scene()
    {
        // ── Camera entity ──────────────────────────────────────
        camera_entity = world.Create_entity();

        // Place camera at (0, 1, -5) looking toward +Z.
        world.Add_component<ECS::Transform_Component>(camera_entity);
        auto& cam_transform =world.Get_component<ECS::Transform_Component>(camera_entity);
   
        cam_transform.Set_position({ 0.0f, 1.0f, 5.0f });
        world.Add_component<ECS::Camera_Component>(camera_entity,
            ECS::Camera_Component::Make_perspective(
                MathLib::Constants::FOV_DEFAULT,
                MathLib::Constants::NEAR_PLANE_DEFAULT,
                MathLib::Constants::FAR_PLANE_DEFAULT));

        transform_system.Register(camera_entity, world);

        std::cout << "[Engine] Camera entity created (id=" << camera_entity << ").\n";

        // ── Sphere primitive ───────────────────────────────────
        // Generate and upload a unit sphere (16 segments, 8 rings).
        ResourceManager::Primitive_Desc sphere_desc;
        sphere_desc.type = ResourceManager::Primitive_Type::Sphere;
        sphere_desc.param1 = 16;   // segments
        sphere_desc.param2 = 8;    // rings

        CoreTypes::Asset_Handle sphere_handle =
            resources.Create_primitive(sphere_desc);

        // Upload to GPU and register the gpu_id.
        const uint32_t sphere_gpu_id =
            renderer.Upload_mesh(resources.Get_mesh_data(sphere_handle));
        resources.Register_gpu_id(sphere_handle, sphere_gpu_id);

        // Create the sphere entity.
        const ECS::Entity sphere_entity = world.Create_entity();

        world.Add_component<ECS::Transform_Component>(sphere_entity);
        world.Get_component<ECS::Transform_Component>(sphere_entity)
            .Set_position({ 0.0f, 0.0f, 0.0f });

        world.Add_component<ECS::Mesh_Component>(sphere_entity,
            ECS::Mesh_Component(sphere_handle));

        transform_system.Register(sphere_entity, world);

        std::cout << "[Engine] Sphere entity created (id=" << sphere_entity << ").\n";

        // ── Directional light ──────────────────────────────────
        const ECS::Entity light_entity = world.Create_entity();

        world.Add_component<ECS::Transform_Component>(light_entity);

        // Rotate the light 45° down and 45° to the right (classic sunlight).
        world.Get_component<ECS::Transform_Component>(light_entity)
            .Set_rotation_euler(
                -MathLib::Constants::QUARTER_PI,   // pitch: -45°
                MathLib::Constants::QUARTER_PI,   // yaw:    45°
                0.0f);

        world.Add_component<ECS::Light_Component>(light_entity,
            ECS::Light_Component::Make_directional(
                { 1.0f, 0.98f, 0.95f },   // warm white sunlight
                1.0f));

        transform_system.Register(light_entity, world);

        std::cout << "[Engine] Directional light entity created (id="
            << light_entity << ").\n";
    }

} // namespace EngineCore