#pragma once

#include <MeshData.hpp>

#include <cstdint>
#include <string>

namespace ResourceManager::Mesh_Optimizer
{

    // =========================================================
    // Optimize_Options
    // =========================================================

    // Which passes to run, and their tuning parameters.
    //
    // The passes are NOT independent: each one assumes the previous ones
    // already ran. Turning one off in the middle is meant for debugging
    // ("which pass broke my mesh?"), not for production use.
    struct Optimize_Options
    {
        // Merges binary-identical vertices and rewrites the indices to
        // match. glTF exporters duplicate vertices heavily at UV seams,
        // and Primitive_Builder pushes fresh corners per quad ring, so
        // both of our mesh sources have real work for this pass.
        bool remap_vertices = true;

        // Reorders triangles so the GPU re-runs the vertex shader less
        // often. Touches indices only.
        bool optimize_vertex_cache = true;

        // Reorders triangles so fewer pixels get shaded twice. Requires
        // optimize_vertex_cache: meshoptimizer documents cache-optimized
        // input as a precondition, not a preference, so this pass is
        // skipped if that one is off.
        bool optimize_overdraw = true;

        // Reorders the vertex buffer so it is read close to sequentially,
        // rewriting vertices AND indices. Runs last: the optimal vertex
        // order depends on the final triangle order.
        bool optimize_vertex_fetch = true;

        // How much vertex cache efficiency the overdraw pass is allowed
        // to trade away. 1.05 = the resulting ACMR may end up at most 5%
        // worse than before that pass.
        float overdraw_threshold = 1.05f;
    };

    // =========================================================
    // Mesh_Stats
    // =========================================================

    // One measurement of a mesh. meshoptimizer's analyzers use simplified
    // cache models, so the absolute numbers do not predict any specific
    // GPU -- the before/after delta is the part worth reading.
    //
    // Careful with the RATIO metrics (atvr, overfetch): both are normalized
    // by the vertex count, and the remap pass changes the vertex count. A
    // fully duplicated mesh scores a perfect atvr of 1.0 while transforming
    // three times as many vertices, so those two can read WORSE after a
    // successful optimization. Compare the absolute fields instead
    // (vertices_transformed, bytes_fetched), or acmr, which is normalized
    // by triangle count -- and the triangle count is exactly what these
    // passes leave alone.
    struct Mesh_Stats
    {
        uint32_t vertex_count = 0;
        uint32_t index_count = 0;

        // Vertex shader invocations predicted for one draw of this mesh.
        uint32_t vertices_transformed = 0;

        // Transformed vertices / triangle count. Best case 0.5.
        float acmr = 0.0f;

        // Transformed vertices / vertex count. 1.0 is ideal, but only
        // comparable between meshes of equal vertex count (see above).
        float atvr = 0.0f;

        // Bytes read from the vertex buffer for one draw.
        uint32_t bytes_fetched = 0;

        // Fetched bytes / vertex buffer size. 1.0 is ideal, same caveat.
        float overfetch = 0.0f;

        // Shaded pixels / covered pixels. 1.0 is ideal. Does not depend on
        // the vertex count, so it is directly comparable.
        float overdraw = 0.0f;
    };

    struct Optimize_Stats
    {
        Mesh_Stats before;
        Mesh_Stats after;
    };

    // =========================================================
    // Analyze
    // =========================================================

    // Measures _mesh without modifying it. An empty mesh reports zeroes.
    Mesh_Stats Analyze(const CoreTypes::MeshData& _mesh);

    // =========================================================
    // Optimize
    // =========================================================

    // Optimizes _mesh in place and returns before/after statistics.
    //
    // Pass order is fixed, and it matters:
    //   1. remap        - merge binary-identical vertices
    //   2. vertex cache - reorder triangles for vertex reuse
    //   3. overdraw     - reorder triangles to shade fewer pixels
    //   4. vertex fetch - reorder vertices for sequential reads
    //
    // Running 4 before 3 wastes the fetch pass, because the overdraw pass
    // reorders the triangles again afterwards. Running 3 without 2 feeds
    // the overdraw optimizer input it is documented not to accept.
    //
    // The mesh renders identically afterwards: every pass is a permutation
    // of the same geometry. index_count never changes; vertex_count can
    // only shrink (duplicates merged, unreferenced vertices dropped).
    //
    // MeshData::index_type is recomputed from the final vertex count so
    // Mesh_GPU can downcast the index buffer to uint16_t when it fits.
    //
    // A mesh with no vertices or no indices is returned untouched.
    Optimize_Stats Optimize(CoreTypes::MeshData& _mesh,
        const Optimize_Options& _options = Optimize_Options{});

    // =========================================================
    // Log_stats
    // =========================================================

    // Prints a before/after summary in the engine's log style.
    // _source is the asset path or primitive description, so a line in
    // the log can be traced back to the mesh it came from.
    void Log_stats(const Optimize_Stats& _stats, const std::string& _source);

} // namespace ResourceManager::Mesh_Optimizer