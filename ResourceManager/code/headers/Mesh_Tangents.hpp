#pragma once

#include <MeshData.hpp>

namespace ResourceManager::Mesh_Tangents
{

    // Fills vertex.tangent of every vertex from the positions, normals,
    // UVs and indices already stored in the mesh, using the standard
    // Lengyel method (gradient of the UV over each triangle, accumulated
    // per vertex).
    //
    //   tangent.xyz  unit tangent, perpendicular to the vertex normal.
    //   tangent.w    bitangent sign (+1 / -1): the shader rebuilds the
    //                bitangent as cross(normal, tangent.xyz) * w.
    //
    // Conventions, shared by Primitive_Builder and Mesh_Loader so a
    // generated and a loaded mesh go through the same tangent frame:
    //   - The UVs are the STORED ones (V = 0 is the top edge of the image).
    //   - The bitangent points toward decreasing V, i.e. up in the image
    //     (+Y of a glTF normal map), which is what glTF's tangent.w assumes.
    //     On an unmirrored mapping w comes out +1.
    //
    // A vertex whose triangles give no usable gradient (no UVs, collinear
    // UVs) still gets a finite, normal-perpendicular tangent with w = +1,
    // so a mesh without UVs can be uploaded and shaded without NaNs.
    //
    // Requires normals to be set and unit length. Throws
    // std::invalid_argument if an index addresses a missing vertex.
    void Compute(CoreTypes::MeshData& _mesh);

} // namespace ResourceManager::Mesh_Tangents
