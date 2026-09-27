#include <Engine.hpp>

#include <Transform_Component.hpp>
#include <Camera_Component.hpp>
#include <Mesh_Component.hpp>
#include <Material_Component.hpp>
#include <Light_Component.hpp>
#include <Primitive_Desc.hpp>
#include <MathConstants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace EngineCore
{
    namespace
    {
        // ── Test scene of the GPU-driven roadmap ───────────────────────
        // Clustered lighting needs many point lights and GPU culling needs
        // thousands of objects, most of them outside the view at any time.
        // TEST_SCENE_ENABLED = false leaves only the reference scene.
        constexpr bool     TEST_SCENE_ENABLED = true;

        // Objects per side of the grid (48 x 48 = 2304 objects), world units
        // between neighbours, and the z of the row closest to the camera,
        // in front of the reference objects and behind them in view order.
        constexpr uint32_t TEST_GRID_SIDE = 48;
        constexpr float    TEST_GRID_SPACING = 2.0f;
        constexpr float    TEST_GRID_FIRST_Z = -6.0f;

        // One object out of TEST_TRANSPARENT_EVERY is transparent (drawn by
        // the CPU path, culled on the CPU).
        constexpr uint32_t TEST_TRANSPARENT_EVERY = 13;

        // Height of the floor plane; the reference sphere rests on it.
        constexpr float    TEST_FLOOR_Y = -1.0f;

        // Point lights scattered over the grid, and their range bounds.
        constexpr uint32_t TEST_POINT_LIGHTS = 256;
        constexpr float    TEST_LIGHT_MIN_RANGE = 3.0f;
        constexpr float    TEST_LIGHT_MAX_RANGE = 6.0f;

        // Deterministic value in [0, 1) for element _index and channel
        // _channel (integer hash). Used instead of <random>, whose
        // distributions differ between standard libraries: the test scene
        // must be the same everywhere to serve as a reference.
        float Hash_unit(uint32_t _index, uint32_t _channel)
        {
            uint32_t h = _index * 747796405u + _channel * 2891336453u + 0x9E3779B9u;
            h ^= h >> 16;
            h *= 0x7FEB352Du;
            h ^= h >> 15;
            h *= 0x846CA68Bu;
            h ^= h >> 16;
            return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
        }

        // Fully saturated color of hue _hue in [0, 1).
        MathLib::Vector3 Hue_to_rgb(float _hue)
        {
            const float r = std::clamp(std::abs(_hue * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
            const float g = std::clamp(2.0f - std::abs(_hue * 6.0f - 2.0f), 0.0f, 1.0f);
            const float b = std::clamp(2.0f - std::abs(_hue * 6.0f - 4.0f), 0.0f, 1.0f);
            return { r, g, b };
        }

        // Validation level for the Renderer:
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

#ifdef _MSC_VER
            // MSVC flags std::getenv as unsafe (C4996, an error with SDL
            // checks enabled). _dupenv_s is its checked replacement; it
            // returns a heap copy that has to be released with free().
            char* value = nullptr;
            size_t length = 0;
            const bool requested = _dupenv_s(&value, &length, GPU_AV_ENV_VAR) == 0
                && value != nullptr
                && std::strcmp(value, "1") == 0;
            std::free(value);
#else
            const char* value = std::getenv(GPU_AV_ENV_VAR);
            const bool requested = value != nullptr && std::strcmp(value, "1") == 0;
#endif
            return requested
                ? Renderer_System::Validation_Mode::Gpu_Assisted
                : Renderer_System::Validation_Mode::Standard;
#endif
        }
    }
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
        , renderer(window, Select_validation_mode())   // Off in Release; Standard, or Gpu_Assisted with ENGINE_GPU_AV=1, in Debug; degrades if unavailable

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

    uint32_t Engine::Ensure_material_registered(ECS::Material_Component& _material)
    {
        if (_material.gpu_material_id != ECS::Material_Component::INVALID_GPU_MATERIAL_ID)
            return _material.gpu_material_id;

        Renderer_System::Material_Desc desc;
        desc.base_color = _material.base_color_factor;
        desc.sampler = _material.sampler;

        // Albedo not assigned: White (the Material_Desc default). Assigned
        // but with no GPU index (never uploaded, or a stale handle): Error,
        // so the mistake shows up magenta instead of silently white.
        if (_material.albedo.Is_valid())
        {
            const uint32_t texture_index = resources.Get_image_gpu_id(_material.albedo);

            desc.albedo_texture_index = texture_index != ResourceManager::Resource_Manager::INVALID_GPU_ID
                                        ? texture_index : CoreTypes::Default_Texture::Error;
        }

        _material.gpu_material_id = renderer.Register_material(desc);

        return _material.gpu_material_id;
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
        // Registered after its texture is uploaded: the albedo handle is
        // resolved to its bindless index at this point, once.
        Ensure_material_registered(sphere_material);

        world.Add_component<ECS::Material_Component>(sphere_entity, sphere_material);

        std::cout << "[Engine] Sphere entity created (id=" << sphere_entity << ").\n";

        // ── Cube primitive ─────────────────────────────────────
        // Second primitive family (flat faces), so both winding groups of
        // Primitive_Builder are on screen at once.
        const CoreTypes::Asset_Handle cube_handle =  resources.Create_primitive(ResourceManager::Primitive_Desc::Make_cube());

        const ECS::Entity cube_entity = Spawn_mesh_entity(cube_handle, { -1.5f, 0.0f, 0.0f });

        // Material with the compute-generated texture as albedo. The image
        // is written every frame by procedural.comp and registered as an
        // external image: it follows the same path as the UV checker from
        // Material_Component onwards, with no CPU pixels to upload.
        ECS::Material_Component cube_material;
        cube_material.albedo = resources.Register_external_image("procedural", renderer.Get_procedural_texture_index());

        Ensure_material_registered(cube_material);

        world.Add_component<ECS::Material_Component>(cube_entity, cube_material);

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

        if (TEST_SCENE_ENABLED)
            Setup_test_scene();
    }

    // =========================================================
    // Setup_test_scene: stress content for clustering and culling
    // =========================================================

    void Engine::Setup_test_scene()
    {
        const CoreTypes::Asset_Handle cube_handle = resources.Create_primitive(ResourceManager::Primitive_Desc::Make_cube());
        const CoreTypes::Asset_Handle sphere_handle = resources.Create_primitive(ResourceManager::Primitive_Desc::Make_sphere(16, 8));
        const CoreTypes::Asset_Handle plane_handle = resources.Create_primitive(ResourceManager::Primitive_Desc::Make_plane(1));

        const float grid_width = static_cast<float>(TEST_GRID_SIDE - 1) * TEST_GRID_SPACING;
        const float grid_center_z = TEST_GRID_FIRST_Z - 0.5f * grid_width;

        // ── Floor ──────────────────────────────────────────────
        // A unit plane scaled to cover the grid and the reference objects.
        {
            const ECS::Entity floor_entity = Spawn_mesh_entity(plane_handle, { 0.0f, TEST_FLOOR_Y, grid_center_z });

            const float floor_size = grid_width + 24.0f;
            world.Get_component<ECS::Transform_Component>(floor_entity).Set_scale({ floor_size, 1.0f, floor_size });

            ECS::Material_Component floor_material;
            floor_material.base_color_factor = { 0.55f, 0.55f, 0.55f, 1.0f };
            Ensure_material_registered(floor_material);

            world.Add_component<ECS::Material_Component>(floor_entity, floor_material);
        }

        // ── Materials ──────────────────────────────────────────
        // A small palette shared by the grid: equal materials share one
        // slot of the material table.
        std::array<ECS::Material_Component, 6> palette;

        for (uint32_t i = 0; i < palette.size(); ++i)
        {
            const MathLib::Vector3 tint = glm::mix(Hue_to_rgb(static_cast<float>(i) / static_cast<float>(palette.size())),
                                                   MathLib::Vector3(1.0f), 0.55f);
            palette[i].base_color_factor = MathLib::Vector4(tint, 1.0f);
            Ensure_material_registered(palette[i]);
        }

        // Alpha below one routes the items to the transparent pass.
        ECS::Material_Component glass_material;
        glass_material.base_color_factor = { 0.55f, 0.8f, 1.0f, 0.45f };
        Ensure_material_registered(glass_material);

        // ── Object grid ────────────────────────────────────────
        // Spheres and cubes alternate. Cubes are stretched vertically by a
        // different amount each: the culling radius must follow the largest
        // axis of a non-uniform scale.
        uint32_t transparent_objects = 0;

        for (uint32_t row = 0; row < TEST_GRID_SIDE; ++row)
        {
            for (uint32_t column = 0; column < TEST_GRID_SIDE; ++column)
            {
                const uint32_t index = row * TEST_GRID_SIDE + column;
                const bool     is_sphere = ((row + column) % 2) == 0;

                const float x = (static_cast<float>(column) - 0.5f * static_cast<float>(TEST_GRID_SIDE - 1)) * TEST_GRID_SPACING;
                const float z = TEST_GRID_FIRST_Z - static_cast<float>(row) * TEST_GRID_SPACING;

                MathLib::Vector3 scale;
                float            half_height;

                if (is_sphere)
                {
                    // The sphere primitive has radius 1.
                    const float radius = 0.35f + 0.2f * Hash_unit(index, 0);
                    scale = { radius, radius, radius };
                    half_height = radius;
                }
                else
                {
                    // The cube primitive has side 1.
                    const float height = 0.6f + 1.8f * Hash_unit(index, 1);
                    scale = { 0.7f, height, 0.7f };
                    half_height = 0.5f * height;
                }

                const ECS::Entity entity = Spawn_mesh_entity(is_sphere ? sphere_handle : cube_handle,
                                                             { x, TEST_FLOOR_Y + half_height, z });

                world.Get_component<ECS::Transform_Component>(entity).Set_scale(scale);

                const bool transparent = (index % TEST_TRANSPARENT_EVERY) == 0;
                transparent_objects += transparent ? 1u : 0u;

                world.Add_component<ECS::Material_Component>(entity,
                    transparent ? glass_material : palette[index % palette.size()]);
            }
        }

        // ── Point lights ───────────────────────────────────────
        // Scattered over the grid, close to the objects, with a finite
        // range: a point light with range 0 would reach no cluster.
        for (uint32_t i = 0; i < TEST_POINT_LIGHTS; ++i)
        {
            const float x = (Hash_unit(i, 10) - 0.5f) * (grid_width + TEST_GRID_SPACING);
            const float z = TEST_GRID_FIRST_Z + TEST_GRID_SPACING - Hash_unit(i, 11) * (grid_width + 2.0f * TEST_GRID_SPACING);
            const float y = TEST_FLOOR_Y + 0.4f + 1.4f * Hash_unit(i, 12);

            const MathLib::Vector3 color = glm::mix(Hue_to_rgb(Hash_unit(i, 13)), MathLib::Vector3(1.0f), 0.2f);
            const float            intensity = 2.0f + 2.0f * Hash_unit(i, 14);
            const float            range = TEST_LIGHT_MIN_RANGE + (TEST_LIGHT_MAX_RANGE - TEST_LIGHT_MIN_RANGE) * Hash_unit(i, 15);

            const ECS::Entity light_entity = world.Create_entity();

            world.Add_component<ECS::Transform_Component>(light_entity).Set_position({ x, y, z });
            world.Add_component<ECS::Light_Component>(light_entity, ECS::Light_Component::Make_point(color, intensity, range));

            transform_system.Register(light_entity, world);
        }

        std::cout << "[Engine] Test scene: " << (TEST_GRID_SIDE * TEST_GRID_SIDE) << " objects (" << transparent_objects
            << " transparent) on a floor, " << TEST_POINT_LIGHTS << " point lights.\n";
    }

} // namespace EngineCore
