#include <Mesh_Tangents.hpp>

#include <Vector.hpp>
#include <Vector3.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace ResourceManager::Mesh_Tangents
{

    namespace
    {
        using Vec2 = MathLib::Vector2;
        using Vec3 = MathLib::Vector3;

        // A unit vector perpendicular to the unit vector _n. Deterministic:
        // the world axis least aligned with _n, with its component along _n
        // removed.
        Vec3 Any_perpendicular(const Vec3& _n)
        {
            const Vec3 axis = (std::fabs(_n.x) < 0.9f) ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);

            return MathLib::Vec3::Normalize(axis - _n * MathLib::Vec3::Dot(_n, axis));
        }
    }

    void Compute(CoreTypes::MeshData& _mesh)
    {
        const size_t vertex_count = _mesh.vertices.size();

        std::vector<Vec3> tan_accum(vertex_count, { 0.0f, 0.0f, 0.0f });
        std::vector<Vec3> bitan_accum(vertex_count, { 0.0f, 0.0f, 0.0f });

        // Accumulate tangent/bitangent contributions per triangle.
        //
        // Solves the 2x2 system that relates the triangle's 3D edges to
        // its UV deltas:   e1 = T*du1 + B*dv1
        //                  e2 = T*du2 + B*dv2
        // whose inverse is the 1/denom factor below.
        for (size_t i = 0; i + 2 < _mesh.indices.size(); i += 3)
        {
            const uint32_t i0 = _mesh.indices[i + 0];
            const uint32_t i1 = _mesh.indices[i + 1];
            const uint32_t i2 = _mesh.indices[i + 2];

            if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
                throw std::invalid_argument("Mesh_Tangents::Compute: triangle " + std::to_string(i / 3) +
                    " references a vertex out of range");

            const CoreTypes::Vertex_Static_Mesh_CPU& v0 = _mesh.vertices[i0];
            const CoreTypes::Vertex_Static_Mesh_CPU& v1 = _mesh.vertices[i1];
            const CoreTypes::Vertex_Static_Mesh_CPU& v2 = _mesh.vertices[i2];

            const Vec3 e1 = v1.position - v0.position;
            const Vec3 e2 = v2.position - v0.position;

            const Vec2 d1 = v1.uv - v0.uv;   // d1.x = du1, d1.y = dv1
            const Vec2 d2 = v2.uv - v0.uv;   // d2.x = du2, d2.y = dv2

            // denom is twice the triangle's area in UV space. Near zero
            // means the three UVs are collinear: the system has no
            // solution, so f = 0 and this triangle contributes nothing.
            const float denom = d1.x * d2.y - d2.x * d1.y;
            const float f = (std::fabs(denom) < 1e-8f) ? 0.0f : (1.0f / denom);

            const Vec3 tangent = f * (d2.y * e1 - d1.y * e2);

            // The bitangent points toward DEcreasing V, i.e. UP in the
            // image (+Y of a glTF normal map): that is the direction
            // glTF's tangent.w assumes, so primitives and loaded meshes
            // share one tangent frame. It is minus dP/dV, because V = 0
            // is the top edge. On an unmirrored mapping w comes out +1.
            const Vec3 bitangent = f * (d2.x * e1 - d1.x * e2);

            for (uint32_t idx : { i0, i1, i2 })
            {
                tan_accum[idx] += tangent;
                bitan_accum[idx] += bitangent;
            }
        }

        // Orthonormalize each tangent against its normal (Gram-Schmidt)
        // and compute the handedness sign for the bitangent.
        for (size_t i = 0; i < vertex_count; ++i)
        {
            const Vec3& n = _mesh.vertices[i].normal;

            // Strip the component along the normal: averaging across
            // triangles leaves the accumulated tangent non-perpendicular.
            Vec3 tangent = tan_accum[i] - n * MathLib::Vec3::Dot(n, tan_accum[i]);

            // NOT a bare normalize: a zero-length tangent (a vertex whose
            // triangles are all degenerate in UV) would normalize to NaN,
            // and a NaN here reaches the vertex buffer and lights the
            // surface with garbage. The fallback is arbitrary but finite,
            // and perpendicular to the normal like every other tangent.
            const float len = MathLib::Vec3::Length(tangent);
            tangent = (len > 1e-8f) ? tangent / len : Any_perpendicular(n);

            // The bitangent is not stored: the shader rebuilds it as
            // cross(N, T) * w. w is that reconstruction's sign.
            const float sign = (MathLib::Vec3::Dot(MathLib::Vec3::Cross(n, tangent), bitan_accum[i]) < 0.0f) ? -1.0f : 1.0f;

            _mesh.vertices[i].tangent = MathLib::Vector4(tangent, sign);
        }
    }

} // namespace ResourceManager::Mesh_Tangents
