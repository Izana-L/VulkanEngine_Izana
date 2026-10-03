#pragma once

#include <Vertex.hpp>

#include <cstdint>
#include <vector>

namespace CoreTypes
{

    // MeshData: CPU-side geometry produced by ResourceManager
    // (via tinygltf or AssetCooker) and consumed by the Renderer
    // to create a vertex buffer + index buffer on the GPU.
    //
    // Vertex layout: interleaved AoS (Array of Structs), one
    // Vertex_Static_Mesh per element. If separate vertex streams
    // are needed in the future (SoA for SIMD skinning, etc.), that
    // is a MeshData + pipeline change — nothing else.
    //
    // indices: always uint32_t, on the CPU and on the GPU. The geometry
    //   pool has a single index type (VK_INDEX_TYPE_UINT32), so there is no
    //   per-mesh index width to record: whatever width the source file used
    //   is widened when the mesh is loaded.
    struct MeshData
    {
        std::vector< Vertex_Static_Mesh_CPU  > vertices;
        std::vector< uint32_t >           indices;
    };

} // namespace CoreTypes