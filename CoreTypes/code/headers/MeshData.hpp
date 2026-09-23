#pragma once

#include <Vertex.hpp>

#include <cstdint>
#include <vector>

namespace CoreTypes
{

    // Index_Type: width of index values stored in the index buffer.
    // The Renderer decides whether to keep them as UINT32 or downcast
    // to UINT16 at upload time (if vertex count < 65536, UINT16 saves
    // bandwidth and fits better in GPU caches).
    enum class Index_Type : uint8_t
    {
        UINT16,
        UINT32,
    };

    // MeshData: CPU-side geometry produced by ResourceManager
    // (via tinygltf or AssetCooker) and consumed by the Renderer
    // to create a vertex buffer + index buffer on the GPU.
    //
    // Vertex layout: interleaved AoS (Array of Structs), one
    // Vertex_Static_Mesh per element. If separate vertex streams
    // are needed in the future (SoA for SIMD skinning, etc.), that
    // is a MeshData + pipeline change — nothing else.
    //
    // indices: always UINT32 on the CPU side for simplicity.
    //   The Renderer may compress to UINT16 at upload if vertex_count
    //   fits. index_type records what the Renderer actually stored.
    struct MeshData
    {
        std::vector< Vertex_Static_Mesh_CPU  > vertices;
        std::vector< uint32_t >           indices;
        Index_Type                        index_type = Index_Type::UINT32;
    };

} // namespace CoreTypes