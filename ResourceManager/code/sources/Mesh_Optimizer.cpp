#include <Mesh_Optimizer.hpp>

#include <meshoptimizer.h>

#include <iostream>
#include <utility>
#include <vector>

namespace ResourceManager::Mesh_Optimizer
{

    // =========================================================
    // Internal helpers
    // =========================================================

    namespace
    {
        using Vertex = CoreTypes::Vertex_Static_Mesh_CPU;

        // meshopt_generateVertexRemap decides whether two vertices are the
        // same by comparing all sizeof(Vertex) bytes. Padding bytes are
        // never written by Mesh_Loader or Primitive_Builder, so a gap in
        // the layout would make deduplication depend on whatever happened
        // to be in memory.
        //
        // This fails the build the day someone adds a field that
        // introduces padding, or turns on GLM's aligned types.
        static_assert(sizeof(Vertex) == sizeof(MathLib::Vector3) * 2 + sizeof(MathLib::Vector4) * 2+ sizeof(MathLib::Vector2),
                        "Vertex_Static_Mesh contains padding bytes. meshopt_generateVertexRemap "
                        "deduplicates with a byte comparison, so padding would have to be "
                        "zero-initialized for the result to be deterministic. Either remove the "
                        "padding or switch to meshopt_generateVertexRemapCustom.");

        // Analyzer cache model. 16/0/0 is meshoptimizer's vendor-neutral
        // configuration; per-vendor numbers exist (NVIDIA 32/32/32, AMD
        // 14/64/128), but one neutral ATVR is what a before/after delta
        // needs.
        constexpr unsigned int ANALYZER_CACHE_SIZE = 16;
        constexpr unsigned int ANALYZER_WARP_SIZE = 0;
        constexpr unsigned int ANALYZER_PRIMGROUP_SIZE = 0;

        // Highest vertex count whose indices still fit in a uint16_t.
        // 65536 vertices means a maximum index of 65535, which fits.
        constexpr size_t UINT16_VERTEX_LIMIT = 65536;

        // meshoptimizer reads positions as a float3 in the first 12 bytes
        // of each vertex, walked by stride. Vertex_Static_Mesh declares
        // position first, so this is the vertex buffer's base pointer --
        // spelled out through the member so it breaks loudly if the field
        // order ever changes.
        const float* Position_stream(const CoreTypes::MeshData& _mesh)
        {
            return &_mesh.vertices[0].position.x;
        }
    }

    // =========================================================
    // Analyze
    // =========================================================

    Mesh_Stats Analyze(const CoreTypes::MeshData& _mesh)
    {
        Mesh_Stats stats;

        stats.vertex_count = static_cast<uint32_t>(_mesh.vertices.size());
        stats.index_count = static_cast<uint32_t>(_mesh.indices.size());

        if (_mesh.vertices.empty() || _mesh.indices.empty())
            return stats;

        const meshopt_VertexCacheStatistics cache = meshopt_analyzeVertexCache(_mesh.indices.data(),_mesh.indices.size(),
                                                                               _mesh.vertices.size(),ANALYZER_CACHE_SIZE, 
                                                                               ANALYZER_WARP_SIZE, ANALYZER_PRIMGROUP_SIZE);
            

        const meshopt_VertexFetchStatistics fetch = meshopt_analyzeVertexFetch(_mesh.indices.data(), _mesh.indices.size(),
                                                                               _mesh.vertices.size(), sizeof(CoreTypes::Vertex_Static_Mesh));

        const meshopt_OverdrawStatistics overdraw = meshopt_analyzeOverdraw( _mesh.indices.data(),  _mesh.indices.size(),
                                                                             Position_stream(_mesh),_mesh.vertices.size(),sizeof(Vertex));

        stats.vertices_transformed = cache.vertices_transformed;
        stats.acmr = cache.acmr;
        stats.atvr = cache.atvr;
        stats.bytes_fetched = fetch.bytes_fetched;
        stats.overfetch = fetch.overfetch;
        stats.overdraw = overdraw.overdraw;

        return stats;
    }

    // =========================================================
    // Optimize
    // =========================================================

    Optimize_Stats Optimize(CoreTypes::MeshData& _mesh,
        const Optimize_Options& _options)
    {
        Optimize_Stats stats;
        stats.before = Analyze(_mesh);

        // Nothing to do, and every meshopt_* call below would be indexing
        // into an empty vector (Position_stream() dereferences vertices[0]).
        if (_mesh.vertices.empty() || _mesh.indices.empty())
        {
            stats.after = stats.before;
            return stats;
        }

        // ── 1. Remap: merge binary-identical vertices ─────────
        if (_options.remap_vertices)
        {
            // The table maps OLD vertex index -> NEW vertex index, so it
            // has one entry per VERTEX, not per index. (meshoptimizer's
            // own example sizes it by index count because its input is
            // unindexed, where the two happen to be equal. Ours is not.)
            std::vector<unsigned int> remap(_mesh.vertices.size());

            const size_t unique_vertex_count = meshopt_generateVertexRemap(remap.data(), _mesh.indices.data(),
                                                                           _mesh.indices.size(), _mesh.vertices.data(), 
                                                                           _mesh.vertices.size(),sizeof(Vertex));
            // Indices first. remapVertexBuffer wants the ORIGINAL vertex
            // count, so shrinking the vertex vector has to come after both
            // remaps. Writing the indices onto themselves is safe: the
            // remap is element-wise, destination[i] from indices[i].
            meshopt_remapIndexBuffer( _mesh.indices.data(),_mesh.indices.data(),_mesh.indices.size(),remap.data());
                
            std::vector<Vertex> remapped(unique_vertex_count);

            meshopt_remapVertexBuffer( remapped.data(), _mesh.vertices.data(), _mesh.vertices.size(),sizeof(Vertex), remap.data());

            _mesh.vertices = std::move(remapped);
        }

        // ── 2. Vertex cache ───────────────────────────────────
        if (_options.optimize_vertex_cache)
        {
            meshopt_optimizeVertexCache( _mesh.indices.data(), _mesh.indices.data(),_mesh.indices.size(), _mesh.vertices.size());
        }

        // ── 3. Overdraw ───────────────────────────────────────
        // Gated on the cache pass as well: meshoptimizer requires input
        // that is already cache-optimized, so running this one alone
        // would be feeding it data it does not accept.
        if (_options.optimize_overdraw && _options.optimize_vertex_cache)
        {
            meshopt_optimizeOverdraw(_mesh.indices.data(), _mesh.indices.data(),_mesh.indices.size(),Position_stream(_mesh),
                                     _mesh.vertices.size(),sizeof(Vertex), _options.overdraw_threshold);
        }

        // ── 4. Vertex fetch ───────────────────────────────────
        // Rewrites the vertex buffer in place (meshoptimizer copies the
        // source internally when destination == vertices) and patches the
        // indices to match. The return value is how many vertices the
        // index buffer actually references: anything past that is
        // unreachable geometry, so the vector shrinks to drop it.
        if (_options.optimize_vertex_fetch)
        {
            const size_t used_vertex_count = meshopt_optimizeVertexFetch(  _mesh.vertices.data(),_mesh.indices.data(), 
                                                                           _mesh.indices.size(), _mesh.vertices.data(), 
                                                                           _mesh.vertices.size(),sizeof(Vertex));

            _mesh.vertices.resize(used_vertex_count);
        }

        // ── Index width ───────────────────────────────────────
        // Mesh_GPU reads index_type to decide whether to downcast the
        // index buffer to uint16_t at upload time, halving its size.
        // It has to be recomputed here for two reasons: the remap pass
        // can push a mesh under the 16-bit limit that did not fit before,
        // and Primitive_Builder marks everything it generates as UINT32
        // no matter how small it is.
        _mesh.index_type = (_mesh.vertices.size() <= UINT16_VERTEX_LIMIT)? CoreTypes::Index_Type::UINT16 : CoreTypes::Index_Type::UINT32;

        stats.after = Analyze(_mesh);

        return stats;
    }

    // =========================================================
    // Log_stats
    // =========================================================

    void Log_stats(const Optimize_Stats& _stats, const std::string& _source)
    {
        // Only the metrics that stay comparable when the vertex count
        // changes are printed; see the note on Mesh_Stats.
        std::cout
            << "[Mesh_Optimizer] " << _source << "\n"
            << "    vertices    " << _stats.before.vertex_count
            << " -> " << _stats.after.vertex_count << "\n"
            << "    transformed " << _stats.before.vertices_transformed
            << " -> " << _stats.after.vertices_transformed
            << "  (vertex shader invocations)\n"
            << "    ACMR        " << _stats.before.acmr
            << " -> " << _stats.after.acmr << "  (0.5 = ideal)\n"
            << "    fetched     " << _stats.before.bytes_fetched
            << " -> " << _stats.after.bytes_fetched << " bytes\n"
            << "    overdraw    " << _stats.before.overdraw
            << " -> " << _stats.after.overdraw << "  (1.0 = ideal)\n";
    }

} // namespace ResourceManager::Mesh_Optimizer