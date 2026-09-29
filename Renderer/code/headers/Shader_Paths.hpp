#pragma once

#include <Filesystem.hpp>

#include <array>
#include <string>

namespace Renderer_System::Shader_Paths
{

    // Compiled shaders of the Renderer, as paths relative to the asset root
    // (Platform::Filesystem::Get_asset_root, the executable's directory).
    //
    // The build compiles every stage in Renderer/shaders into
    // Renderer/shaders/compiled/<file>.<stage>.spv (Directory.Build.targets
    // of the Renderer project) and deploys that folder next to the
    // executable as shaders/ (Directory.Build.targets of the Game project).
    // Names are lowercase, matching the source files exactly, so the paths
    // also hold on case-sensitive filesystems.
    //
    // Every stage the Renderer loads is listed here and only here: the
    // pipelines take their paths from these constants, and ALL is what the
    // startup check verifies before the Renderer is created.

    inline constexpr const char* MESH_VERT = "shaders/mesh.vert.spv";
    inline constexpr const char* MESH_FRAG = "shaders/mesh.frag.spv";
    inline constexpr const char* MESH_OIT_FRAG = "shaders/mesh_oit.frag.spv";
    inline constexpr const char* BOUNDS_VERT = "shaders/bounds.vert.spv";
    inline constexpr const char* BOUNDS_FRAG = "shaders/bounds.frag.spv";
    inline constexpr const char* OIT_COMPOSITE_VERT = "shaders/oit_composite.vert.spv";
    inline constexpr const char* OIT_COMPOSITE_FRAG = "shaders/oit_composite.frag.spv";
    inline constexpr const char* PROCEDURAL_COMP = "shaders/procedural.comp.spv";
    inline constexpr const char* CULL_OBJECTS_COMP = "shaders/cull_objects.comp.spv";
    inline constexpr const char* CLUSTER_LIGHTS_COMP = "shaders/cluster_lights.comp.spv";

    inline constexpr std::array<const char*, 10> ALL = {
        MESH_VERT, MESH_FRAG, MESH_OIT_FRAG,
        BOUNDS_VERT, BOUNDS_FRAG,
        OIT_COMPOSITE_VERT, OIT_COMPOSITE_FRAG,
        PROCEDURAL_COMP, CULL_OBJECTS_COMP, CLUSTER_LIGHTS_COMP,
    };

    // Absolute path of one of the shaders above.
    inline std::string Resolve(const char* _relative_path)
    {
        return Platform::Filesystem::Resolve_asset_path(_relative_path);
    }

} // namespace Renderer_System::Shader_Paths
