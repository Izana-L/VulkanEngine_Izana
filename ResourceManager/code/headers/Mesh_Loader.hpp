#pragma once

#include <MeshData.hpp>

#include <string>
#include <vector>

namespace ResourceManager::Mesh_Loader
{

    // Loads all meshes from a glTF or GLB file and returns one
    // MeshData per primitive found in the file.
    //
    // Vertex attributes extracted:
    //   POSITION    → Vertex_Static_Mesh::position  (required)
    //   NORMAL      → Vertex_Static_Mesh::normal     (default: 0,1,0 if absent)
    //   TEXCOORD_0  → Vertex_Static_Mesh::uv         (default: 0,0   if absent)
    //   TANGENT     → Vertex_Static_Mesh::tangent    (default: 1,0,0,1 if absent)
    //   COLOR_0     → Vertex_Static_Mesh::color      (default: 1,1,1,1 if absent)
    //
    // Index type:
    //   Respects the original index width from the glTF (UINT16 or UINT32).
    //   Indices are always stored as uint32_t in MeshData::indices regardless,
    //   but MeshData::index_type reflects the original width so Mesh_GPU can
    //   convert back to UINT16 at upload time to save GPU memory.
    //
    // Throws std::runtime_error if the file cannot be opened or parsed,
    // or if a primitive has no POSITION attribute.
    std::vector<CoreTypes::MeshData> Load(const std::string& _path);

} // namespace ResourceManager::Mesh_Loader