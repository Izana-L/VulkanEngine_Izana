#pragma once

#include <MeshData.hpp>

#include <string>
#include <vector>

namespace ResourceManager::Mesh_Loader
{

    // Loads the meshes of a glTF or GLB file and returns one MeshData per
    // primitive instance in the scene.
    //
    // File format: binary glTF is recognised by its first four bytes
    // ("glTF"), not by the file name, so the extension and its case do not
    // matter.
    //
    // Scene: only what the default scene shows is loaded (scene 0 when the
    // file marks none; every root node when the file has no scenes; every
    // mesh when it has no nodes at all). The transform of each node, composed
    // down the hierarchy, is BAKED into the vertices: positions, normals and
    // tangents are returned in world space and the winding of a mirrored
    // (negative-determinant) transform is flipped back to counter-clockwise.
    // A mesh used by several nodes is returned once per node, each copy with
    // its own transform; an instance whose transform is singular (zero
    // scale) is skipped. Meshes no node of the scene uses are skipped, and
    // reported.
    //
    // Primitive modes: TRIANGLES, TRIANGLE_STRIP and TRIANGLE_FAN are all
    // returned as triangle lists, counter-clockwise from outside. POINTS
    // and LINES* have no meaning as a MeshData and are skipped.
    //
    // Vertex attributes extracted:
    //   POSITION    → Vertex_Static_Mesh::position  (required, VEC3)
    //   NORMAL      → Vertex_Static_Mesh::normal     (flat face normals if absent,
    //                                                 as glTF requires)
    //   TEXCOORD_0  → Vertex_Static_Mesh::uv         (default: 0,0   if absent)
    //   TANGENT     → Vertex_Static_Mesh::tangent    (computed from the UVs and
    //                                                 normals if absent; a
    //                                                 normal-perpendicular frame
    //                                                 when there are no UVs)
    //   COLOR_0     → Vertex_Static_Mesh::color      (default: 1,1,1,1 if absent;
    //                                                 a VEC3 color gets alpha 1)
    // Each attribute must have the accessor type glTF fixes for it (POSITION
    // VEC3, TEXCOORD_0 VEC2, ...) and a component type glTF allows for it
    // (FLOAT, normalized integers for UVs and colors, and the quantized types
    // KHR_mesh_quantization adds); anything else is rejected, not guessed.
    //
    // Indices:
    //   The glTF index width (UNSIGNED_BYTE, UNSIGNED_SHORT or UNSIGNED_INT)
    //   is widened to uint32_t in MeshData::indices; the original width is
    //   not kept, because the geometry pool has a single index type. A
    //   primitive without an index accessor is drawn from its vertex array,
    //   so its indices are 0, 1, 2, ...
    //
    // Not imported, and reported with a warning when the file has them:
    // materials, textures and images (MeshData holds geometry only), skins
    // and animations, and vertex attributes other than the ones above.
    //
    // Safety: the file is untrusted input. Every offset, stride and count in
    // it is checked, with overflow-safe arithmetic, against the buffer view
    // and the buffer before a single byte is read.
    //
    // Throws std::runtime_error if the file cannot be opened or parsed, if it
    // is malformed (accessor out of bounds, wrong accessor type, index out of
    // range, cyclic node graph...), if a primitive has no POSITION attribute,
    // or if no triangle primitive remains.
    std::vector<CoreTypes::MeshData> Load(const std::string& _path);

} // namespace ResourceManager::Mesh_Loader
