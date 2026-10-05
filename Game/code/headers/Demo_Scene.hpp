#pragma once

#include <Entity.hpp>
#include <Asset_Handle.hpp>
#include <Vector3.hpp>

#include <string>

namespace ECS { class World; class Transform_System; }
namespace EngineCore { class Engine_Context; class Gpu_Assets; }

namespace Game
{

    // Demo_Scene: the test scene of the GPU-driven roadmap. Deterministic on
    // purpose: it is the visual reference used to compare render paths, so
    // the same scene must come out on every run and on every machine.
    //
    // It builds entities through the Engine_Context (world, assets,
    // transforms) and knows nothing else about the engine. It does not
    // create the camera: that is the Camera_Controller's job, and it must
    // run first, so the camera stays the first entity of the world.
    //
    // Temporary until the editor exists: a scene loader will replace it and
    // the systems will not notice.
    class Demo_Scene
    {
    public:

        // _assets_root: root folder of the game's assets (textures/...).
        Demo_Scene(EngineCore::Engine_Context& _context, const std::string& _assets_root);

        Demo_Scene(const Demo_Scene&) = delete;
        Demo_Scene& operator=(const Demo_Scene&) = delete;

        // Builds the whole scene: the reference objects, the directional
        // light and, if enabled, the stress content below.
        void Build();

    private:

        // Creates an entity with a transform and a mesh at _position. The
        // mesh must already be uploaded (Gpu_Assets::Create_primitive).
        ECS::Entity Spawn_mesh_entity(CoreTypes::Asset_Handle _mesh, const MathLib::Vector3& _position);

        // Creates the reference scene: a textured sphere, a cube and a
        // directional light. Then adds the test scene.
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

        ECS::World& world;
        EngineCore::Gpu_Assets& assets;
        ECS::Transform_System& transform_system;
        std::string             assets_root;
    };

} // namespace Game