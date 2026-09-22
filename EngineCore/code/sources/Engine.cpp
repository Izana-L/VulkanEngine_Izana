#include <Engine.hpp>

#include <Transform_Component.hpp>
#include <Camera_Component.hpp>
#include <Mesh_Component.hpp>
#include <Material_Component.hpp>
#include <Light_Component.hpp>
#include <Primitive_Desc.hpp>
#include <MathConstants.hpp>

#include <iostream>
#include <stdexcept>

namespace EngineCore
{

    // =========================================================
    // Constructor: builds subsystems in dependency order
    // =========================================================

    Engine::Engine()
        // Layer 0: Foundation
        : window(1280, 720, "Vulkan Engine")

        // Layer 1: Services
        , input(window)
        , resources()

        // Layer 2: GPU
        , renderer(window, true)   // true = request validation layers (disabled if unavailable)

        // Layer 3: Simulation (no constructor args needed)
        , world()
        , transform_system()
        , camera_controller()
        , extractor()

        // Layer 4: Orchestration
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
    // Upload helpers
    // =========================================================

    uint32_t Engine::Ensure_mesh_uploaded(CoreTypes::Asset_Handle _mesh)
    {
        const uint32_t existing = resources.Get_gpu_id(_mesh);
        if (existing != ResourceManager::Resource_Manager::INVALID_GPU_ID)
            return existing;

        const uint32_t gpu_id = renderer.Upload_mesh(resources.Get_mesh_data(_mesh));
        resources.Register_gpu_id(_mesh, gpu_id);

        return gpu_id;
    }

    uint32_t Engine::Ensure_image_uploaded(CoreTypes::Asset_Handle _image)
    {
        const uint32_t existing = resources.Get_image_gpu_id(_image);
        if (existing != ResourceManager::Resource_Manager::INVALID_GPU_ID)
            return existing;

        const uint32_t bindless_index = renderer.Upload_texture(resources.Get_image_data(_image));
        resources.Register_image_gpu_id(_image, bindless_index);

        return bindless_index;
    }

    ECS::Entity Engine::Spawn_mesh_entity(CoreTypes::Asset_Handle _mesh, const MathLib::Vector3& _position)
    {
        Ensure_mesh_uploaded(_mesh);

        const ECS::Entity entity = world.Create_entity();

        world.Add_component<ECS::Transform_Component>(entity).Set_position(_position);
        world.Add_component<ECS::Mesh_Component>(entity, ECS::Mesh_Component(_mesh));

        transform_system.Register(entity, world);

        return entity;
    }

    // =========================================================
    // Setup_scene: test scene
    // =========================================================

    void Engine::Setup_scene()
    {
        // ── Camera entity ──────────────────────────────────────
        camera_entity = world.Create_entity();

        // Place camera at (0, 1, 5) looking toward -Z (the default forward).
        world.Add_component<ECS::Transform_Component>(camera_entity).Set_position({ 0.0f, 1.0f, 5.0f });

        world.Add_component<ECS::Camera_Component>(camera_entity,
            ECS::Camera_Component::Make_perspective(
                MathLib::Constants::FOV_DEFAULT,
                MathLib::Constants::NEAR_PLANE_DEFAULT,
                MathLib::Constants::FAR_PLANE_DEFAULT));

        transform_system.Register(camera_entity, world);

        std::cout << "[Engine] Camera entity created (id=" << camera_entity << ").\n";

        // ── Sphere primitive ───────────────────────────────────
        // A unit sphere (16 segments, 8 rings), built through the named
        // constructor: it takes exactly the two parameters a sphere reads,
        // so no value can end up in a field the generator ignores.
        const CoreTypes::Asset_Handle sphere_handle =
            resources.Create_primitive(ResourceManager::Primitive_Desc::Make_sphere(16, 8));

        const ECS::Entity sphere_entity = Spawn_mesh_entity(sphere_handle, { 1.5f, 0.0f, 0.0f });

        // Material with the UV checker as albedo. The texture travels the
        // whole bindless path: Image_Loader -> Upload_texture -> bindless
        // index -> Material_Component -> push constant -> mesh.frag.
        // A missing file is reported and the sphere stays untextured.
        ECS::Material_Component sphere_material;

        try
        {
            const CoreTypes::Asset_Handle checker =
                resources.Load_image("../../Game/assets/textures/uv-checker.png", CoreTypes::Pixel_Format::RGBA8_SRGB);

            Ensure_image_uploaded(checker);
            sphere_material.albedo = checker;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Engine] Albedo texture not loaded: " << e.what() << "\n";
        }

        world.Add_component<ECS::Material_Component>(sphere_entity, sphere_material);

        std::cout << "[Engine] Sphere entity created (id=" << sphere_entity << ").\n";

        // ── Cube primitive ─────────────────────────────────────
        // Second primitive family (flat faces), so both winding groups of
        // Primitive_Builder are on screen at once.
        const CoreTypes::Asset_Handle cube_handle =
            resources.Create_primitive(ResourceManager::Primitive_Desc::Make_cube());

        const ECS::Entity cube_entity = Spawn_mesh_entity(cube_handle, { -1.5f, 0.0f, 0.0f });

        std::cout << "[Engine] Cube entity created (id=" << cube_entity << ").\n";

        // ── Directional light ──────────────────────────────────
        const ECS::Entity light_entity = world.Create_entity();

        // Rotate the light 45 degrees down and 45 degrees to the right
        // (classic sunlight).
        world.Add_component<ECS::Transform_Component>(light_entity)
            .Set_rotation_euler(
                -MathLib::Constants::QUARTER_PI,   // pitch: -45 degrees
                MathLib::Constants::QUARTER_PI,    // yaw:    45 degrees
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
