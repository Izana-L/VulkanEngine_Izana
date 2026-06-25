#include <Primitive_Builder.hpp>
#include <MathConstants.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ResourceManager::Primitive_Builder
{

    namespace
    {
        

        using Vertex = CoreTypes::Vertex_Static_Mesh;
        using Mesh = CoreTypes::MeshData;

        // =========================================================
        // Tangent computation
        // =========================================================

        // Computes per-vertex tangents from positions, UVs and normals
        // using the standard Lengyel method (gradient of UV over the
        // triangle). Fills vertex.tangent.xyz with the orthonormalized
        // tangent and vertex.tangent.w with the bitangent sign (+1/-1).
        //
        // Call after all positions, normals, UVs and indices are set.
        void Compute_tangents(Mesh& _mesh)
        {
            const size_t vertex_count = _mesh.vertices.size();

            std::vector<MathLib::Vector3> tan_accum(vertex_count, { 0.0f, 0.0f, 0.0f });
            std::vector<MathLib::Vector3> bitan_accum(vertex_count, { 0.0f, 0.0f, 0.0f });

            // Accumulate tangent/bitangent contributions per triangle.
            for (size_t i = 0; i + 2 < _mesh.indices.size(); i += 3)
            {
                const uint32_t i0 = _mesh.indices[i + 0];
                const uint32_t i1 = _mesh.indices[i + 1];
                const uint32_t i2 = _mesh.indices[i + 2];

                const Vertex& v0 = _mesh.vertices[i0];
                const Vertex& v1 = _mesh.vertices[i1];
                const Vertex& v2 = _mesh.vertices[i2];

                const float e1x = v1.position.x - v0.position.x;
                const float e1y = v1.position.y - v0.position.y;
                const float e1z = v1.position.z - v0.position.z;

                const float e2x = v2.position.x - v0.position.x;
                const float e2y = v2.position.y - v0.position.y;
                const float e2z = v2.position.z - v0.position.z;

                const float du1 = v1.uv.x - v0.uv.x;
                const float dv1 = v1.uv.y - v0.uv.y;
                const float du2 = v2.uv.x - v0.uv.x;
                const float dv2 = v2.uv.y - v0.uv.y;

                const float denom = du1 * dv2 - du2 * dv1;
                const float f = (std::fabs(denom) < 1e-8f) ? 0.0f : (1.0f / denom);

                const MathLib::Vector3 tangent{
                    f * (dv2 * e1x - dv1 * e2x),
                    f * (dv2 * e1y - dv1 * e2y),
                    f * (dv2 * e1z - dv1 * e2z)
                };

                const MathLib::Vector3 bitangent{
                    f * (du1 * e2x - du2 * e1x),
                    f * (du1 * e2y - du2 * e1y),
                    f * (du1 * e2z - du2 * e1z)
                };

                for (uint32_t idx : { i0, i1, i2 })
                {
                    tan_accum[idx].x += tangent.x;
                    tan_accum[idx].y += tangent.y;
                    tan_accum[idx].z += tangent.z;
                    bitan_accum[idx].x += bitangent.x;
                    bitan_accum[idx].y += bitangent.y;
                    bitan_accum[idx].z += bitangent.z;
                }
            }

            // Orthonormalize each tangent against its normal (Gram-Schmidt)
            // and compute the handedness sign for the bitangent.
            for (size_t i = 0; i < vertex_count; ++i)
            {
                const MathLib::Vector3& n = _mesh.vertices[i].normal;
                const MathLib::Vector3& t = tan_accum[i];

                // t_proj = t - n * dot(n, t)
                const float n_dot_t = n.x * t.x + n.y * t.y + n.z * t.z;
                MathLib::Vector3 tangent{
                    t.x - n.x * n_dot_t,
                    t.y - n.y * n_dot_t,
                    t.z - n.z * n_dot_t
                };

                const float len = std::sqrt(tangent.x * tangent.x +
                    tangent.y * tangent.y +
                    tangent.z * tangent.z);
                if (len > 1e-8f) {
                    tangent.x /= len;
                    tangent.y /= len;
                    tangent.z /= len;
                }
                else {
                    tangent = { 1.0f, 0.0f, 0.0f };
                }

                // Handedness: sign of dot(cross(n, t), bitangent)
                const MathLib::Vector3& b = bitan_accum[i];
                const MathLib::Vector3 n_cross_t{
                    n.y * tangent.z - n.z * tangent.y,
                    n.z * tangent.x - n.x * tangent.z,
                    n.x * tangent.y - n.y * tangent.x
                };
                const float sign =
                    (n_cross_t.x * b.x + n_cross_t.y * b.y + n_cross_t.z * b.z) < 0.0f
                    ? -1.0f : 1.0f;

                _mesh.vertices[i].tangent = { tangent.x, tangent.y, tangent.z, sign };
            }
        }

        // Sets every vertex color to white (default tint).
        void Set_default_color(Mesh& _mesh)
        {
            for (Vertex& v : _mesh.vertices)
                v.color = { 1.0f, 1.0f, 1.0f, 1.0f };
        }

        // Finalizes a mesh: default color + tangents + UINT32 indices.
        void Finalize(Mesh& _mesh)
        {
            Set_default_color(_mesh);
            Compute_tangents(_mesh);
            _mesh.index_type = CoreTypes::Index_Type::UINT32;
        }

    } // anonymous namespace

    // =========================================================
    // Cube — hardcoded, 24 vertices (4 per face for per-face normals)
    // =========================================================

    CoreTypes::MeshData Build_cube()
    {
        Mesh mesh;

        // Each face: 4 vertices with the same normal, 2 triangles.
        // Layout per face: bottom-left, bottom-right, top-right, top-left.
        struct Face { MathLib::Vector3 normal; MathLib::Vector3 v[4]; };

        const float h = 0.5f;
        const Face faces[6] = {
            // +X
            { { 1, 0, 0}, {{ h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h}} },
            // -X
            { {-1, 0, 0}, {{-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h}} },
            // +Y
            { { 0, 1, 0}, {{-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h}} },
            // -Y
            { { 0,-1, 0}, {{-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h}} },
            // +Z
            { { 0, 0, 1}, {{-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h}} },
            // -Z
            { { 0, 0,-1}, {{ h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h}} },
        };

        const MathLib::Vector2 uvs[4] = { {0,0}, {1,0}, {1,1}, {0,1} };

        for (const Face& face : faces)
        {
            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());

            for (int i = 0; i < 4; ++i)
            {
                Vertex v{};
                v.position = face.v[i];
                v.normal = face.normal;
                v.uv = uvs[i];
                mesh.vertices.push_back(v);
            }

            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 1);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 3);
        }

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Quad — hardcoded, 1x1 in XZ plane facing +Y, centered
    // =========================================================

    CoreTypes::MeshData Build_quad()
    {
        Mesh mesh;

        const float h = 0.5f;
        const MathLib::Vector3 n{ 0, 1, 0 };

        mesh.vertices = {
            { {-h, 0,  h}, n, {}, {0, 0}, {} },
            { { h, 0,  h}, n, {}, {1, 0}, {} },
            { { h, 0, -h}, n, {}, {1, 1}, {} },
            { {-h, 0, -h}, n, {}, {0, 1}, {} },
        };

        mesh.indices = { 0, 1, 2, 0, 2, 3 };

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Triangle — hardcoded, unit triangle in XZ, facing +Y
    // =========================================================

    CoreTypes::MeshData Build_triangle()
    {
        Mesh mesh;

        const MathLib::Vector3 n{ 0, 1, 0 };

        mesh.vertices = {
            { {-0.5f, 0,  0.5f}, n, {}, {0.0f, 0.0f}, {} },
            { { 0.5f, 0,  0.5f}, n, {}, {1.0f, 0.0f}, {} },
            { { 0.0f, 0, -0.5f}, n, {}, {0.5f, 1.0f}, {} },
        };

        mesh.indices = { 0, 1, 2 };

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Tetrahedron — hardcoded, 4 triangular faces, flat-shaded
    // =========================================================

    CoreTypes::MeshData Build_tetrahedron()
    {
        Mesh mesh;

        // Four corners of a regular tetrahedron inscribed in a unit sphere.
        const MathLib::Vector3 a{ 0.0f,        0.6123f,  0.0f };
        const MathLib::Vector3 b{ 0.0f,       -0.2041f,  0.5774f };
        const MathLib::Vector3 c{ -0.5000f,    -0.2041f, -0.2887f };
        const MathLib::Vector3 d{ 0.5000f,    -0.2041f, -0.2887f };

        // Each face is its own 3 vertices for flat normals.
        const MathLib::Vector3 tris[4][3] = {
            { a, b, c },
            { a, c, d },
            { a, d, b },
            { b, d, c },
        };

        const MathLib::Vector2 uvs[3] = { {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f} };

        for (const auto& tri : tris)
        {
            // Face normal = normalized cross of two edges.
            const MathLib::Vector3 e1{ tri[1].x - tri[0].x, tri[1].y - tri[0].y, tri[1].z - tri[0].z };
            const MathLib::Vector3 e2{ tri[2].x - tri[0].x, tri[2].y - tri[0].y, tri[2].z - tri[0].z };

            MathLib::Vector3 normal{
                e1.y * e2.z - e1.z * e2.y,
                e1.z * e2.x - e1.x * e2.z,
                e1.x * e2.y - e1.y * e2.x
            };
            const float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
            if (len > 1e-8f) { normal.x /= len; normal.y /= len; normal.z /= len; }

            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            for (int i = 0; i < 3; ++i)
            {
                Vertex v{};
                v.position = tri[i];
                v.normal = normal;
                v.uv = uvs[i];
                mesh.vertices.push_back(v);
            }
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 1);
            mesh.indices.push_back(base + 2);
        }

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Plane — 1x1 grid in XZ subdivided _subdivisions times
    // =========================================================

    CoreTypes::MeshData Build_plane(uint16_t _subdivisions)
    {
        Mesh mesh;

        const uint32_t divs = (_subdivisions == 0) ? 1u : _subdivisions;
        const uint32_t verts_per_side = divs + 1;
        const MathLib::Vector3 n{ 0, 1, 0 };

        // Vertices on a regular grid from -0.5 to 0.5 in X and Z.
        for (uint32_t z = 0; z < verts_per_side; ++z)
        {
            for (uint32_t x = 0; x < verts_per_side; ++x)
            {
                const float fx = static_cast<float>(x) / static_cast<float>(divs);
                const float fz = static_cast<float>(z) / static_cast<float>(divs);

                Vertex v{};
                v.position = { fx - 0.5f, 0.0f, fz - 0.5f };
                v.normal = n;
                v.uv = { fx, fz };
                mesh.vertices.push_back(v);
            }
        }

        // Two triangles per grid cell.
        for (uint32_t z = 0; z < divs; ++z)
        {
            for (uint32_t x = 0; x < divs; ++x)
            {
                const uint32_t tl = z * verts_per_side + x;
                const uint32_t tr = tl + 1;
                const uint32_t bl = tl + verts_per_side;
                const uint32_t br = bl + 1;

                mesh.indices.push_back(tl);
                mesh.indices.push_back(bl);
                mesh.indices.push_back(br);
                mesh.indices.push_back(tl);
                mesh.indices.push_back(br);
                mesh.indices.push_back(tr);
            }
        }

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Sphere — UV sphere, radius 1, centered
    // =========================================================

    CoreTypes::MeshData Build_sphere(uint16_t _segments, uint16_t _rings)
    {
        Mesh mesh;

        const uint32_t segments = (_segments < 3) ? 3u : _segments;
        const uint32_t rings = (_rings < 2) ? 2u : _rings;

        // Vertices: (rings+1) latitude bands × (segments+1) longitude.
        for (uint32_t r = 0; r <= rings; ++r)
        {
            const float v = static_cast<float>(r) / static_cast<float>(rings);
            const float phi = v * MathLib::Constants::PI;            // 0..PI latitude
            const float y = std::cos(phi);
            const float r_xz = std::sin(phi);

            for (uint32_t s = 0; s <= segments; ++s)
            {
                const float u = static_cast<float>(s) / static_cast<float>(segments);
                const float theta = u * MathLib::Constants::TWO_PI;    // 0..2PI longitude

                const float x = r_xz * std::cos(theta);
                const float z = r_xz * std::sin(theta);

                Vertex vert{};
                vert.position = { x, y, z };
                vert.normal = { x, y, z };        // unit sphere: position == normal
                vert.uv = { u, v };
                mesh.vertices.push_back(vert);
            }
        }

        const uint32_t stride = segments + 1;
        for (uint32_t r = 0; r < rings; ++r)
        {
            for (uint32_t s = 0; s < segments; ++s)
            {
                const uint32_t a = r * stride + s;
                const uint32_t b = a + stride;

                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(a + 1);

                mesh.indices.push_back(a + 1);
                mesh.indices.push_back(b);
                mesh.indices.push_back(b + 1);
            }
        }

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Cone — radius 1 base at y=0, apex at y=1, _segments around
    // =========================================================

    CoreTypes::MeshData Build_cone(uint16_t _segments)
    {
        Mesh mesh;

        const uint32_t segments = (_segments < 3) ? 3u : _segments;

        // Side: apex duplicated per segment for correct normals.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const float t0 = static_cast<float>(s) / static_cast<float>(segments) * MathLib::Constants::TWO_PI;
            const float t1 = static_cast<float>(s + 1) / static_cast<float>(segments) * MathLib::Constants::TWO_PI;

            const MathLib::Vector3 base0{ std::cos(t0), 0.0f, std::sin(t0) };
            const MathLib::Vector3 base1{ std::cos(t1), 0.0f, std::sin(t1) };
            const MathLib::Vector3 apex{ 0.0f, 1.0f, 0.0f };

            // Face normal of this side triangle.
            const MathLib::Vector3 e1{ base1.x - base0.x, base1.y - base0.y, base1.z - base0.z };
            const MathLib::Vector3 e2{ apex.x - base0.x, apex.y - base0.y, apex.z - base0.z };
            MathLib::Vector3 nrm{
                e1.y * e2.z - e1.z * e2.y,
                e1.z * e2.x - e1.x * e2.z,
                e1.x * e2.y - e1.y * e2.x
            };
            const float len = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
            if (len > 1e-8f) { nrm.x /= len; nrm.y /= len; nrm.z /= len; }

            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back({ base0, nrm, {}, { static_cast<float>(s) / segments,       0.0f }, {} });
            mesh.vertices.push_back({ base1, nrm, {}, { static_cast<float>(s + 1) / segments,     0.0f }, {} });
            mesh.vertices.push_back({ apex,  nrm, {}, { (static_cast<float>(s) + 0.5f) / segments, 1.0f }, {} });

            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 1);
            mesh.indices.push_back(base + 2);
        }

        // Base cap (facing -Y), center fan.
        const uint32_t center = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({ {0,0,0}, {0,-1,0}, {}, {0.5f, 0.5f}, {} });

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float t = static_cast<float>(s) / static_cast<float>(segments) * MathLib::Constants::TWO_PI;
            const float x = std::cos(t);
            const float z = std::sin(t);
            mesh.vertices.push_back({ {x,0,z}, {0,-1,0}, {}, { x * 0.5f + 0.5f, z * 0.5f + 0.5f }, {} });
        }

        for (uint32_t s = 0; s < segments; ++s)
        {
            mesh.indices.push_back(center);
            mesh.indices.push_back(center + s + 2);
            mesh.indices.push_back(center + s + 1);
        }

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Cylinder — radius 1, y from 0 to 1, _segments around
    // =========================================================

    CoreTypes::MeshData Build_cylinder(uint16_t _segments)
    {
        Mesh mesh;

        const uint32_t segments = (_segments < 3) ? 3u : _segments;

        // Side wall.
        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float t = u * MathLib::Constants::TWO_PI;
            const float x = std::cos(t);
            const float z = std::sin(t);
            const MathLib::Vector3 nrm{ x, 0.0f, z };

            mesh.vertices.push_back({ {x, 0.0f, z}, nrm, {}, {u, 0.0f}, {} });
            mesh.vertices.push_back({ {x, 1.0f, z}, nrm, {}, {u, 1.0f}, {} });
        }

        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t a = s * 2;
            const uint32_t b = a + 1;
            const uint32_t c = a + 2;
            const uint32_t d = a + 3;

            mesh.indices.push_back(a);
            mesh.indices.push_back(c);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
            mesh.indices.push_back(d);
        }

        // Top cap (+Y).
        const uint32_t top_center = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({ {0,1,0}, {0,1,0}, {}, {0.5f,0.5f}, {} });
        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float t = static_cast<float>(s) / static_cast<float>(segments) * MathLib::Constants::TWO_PI;
            const float x = std::cos(t);
            const float z = std::sin(t);
            mesh.vertices.push_back({ {x,1,z}, {0,1,0}, {}, {x * 0.5f + 0.5f, z * 0.5f + 0.5f}, {} });
        }
        for (uint32_t s = 0; s < segments; ++s)
        {
            mesh.indices.push_back(top_center);
            mesh.indices.push_back(top_center + s + 1);
            mesh.indices.push_back(top_center + s + 2);
        }

        // Bottom cap (-Y).
        const uint32_t bot_center = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({ {0,0,0}, {0,-1,0}, {}, {0.5f,0.5f}, {} });
        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float t = static_cast<float>(s) / static_cast<float>(segments) * MathLib::Constants::TWO_PI;
            const float x = std::cos(t);
            const float z = std::sin(t);
            mesh.vertices.push_back({ {x,0,z}, {0,-1,0}, {}, {x * 0.5f + 0.5f, z * 0.5f + 0.5f}, {} });
        }
        for (uint32_t s = 0; s < segments; ++s)
        {
            mesh.indices.push_back(bot_center);
            mesh.indices.push_back(bot_center + s + 2);
            mesh.indices.push_back(bot_center + s + 1);
        }

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Torus — outer radius 1, tube radius 0.25
    // =========================================================

    CoreTypes::MeshData Build_torus(uint16_t _segments, uint16_t _rings)
    {
        Mesh mesh;

        const uint32_t segments = (_segments < 3) ? 3u : _segments;  // around the main ring
        const uint32_t rings = (_rings < 3) ? 3u : _rings;     // around the tube

        const float main_radius = 0.75f;   // so outer radius = 1.0
        const float tube_radius = 0.25f;

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float theta = u * MathLib::Constants::TWO_PI;
            const float cos_t = std::cos(theta);
            const float sin_t = std::sin(theta);

            for (uint32_t r = 0; r <= rings; ++r)
            {
                const float v = static_cast<float>(r) / static_cast<float>(rings);
                const float phi = v * MathLib::Constants::TWO_PI;
                const float cos_p = std::cos(phi);
                const float sin_p = std::sin(phi);

                const float x = (main_radius + tube_radius * cos_p) * cos_t;
                const float y = tube_radius * sin_p;
                const float z = (main_radius + tube_radius * cos_p) * sin_t;

                // Normal points outward from the tube center.
                const MathLib::Vector3 nrm{ cos_p * cos_t, sin_p, cos_p * sin_t };

                mesh.vertices.push_back({ {x, y, z}, nrm, {}, {u, v}, {} });
            }
        }

        const uint32_t stride = rings + 1;
        for (uint32_t s = 0; s < segments; ++s)
        {
            for (uint32_t r = 0; r < rings; ++r)
            {
                const uint32_t a = s * stride + r;
                const uint32_t b = a + stride;

                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(a + 1);

                mesh.indices.push_back(a + 1);
                mesh.indices.push_back(b);
                mesh.indices.push_back(b + 1);
            }
        }

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Capsule — radius 1, cylinder height 1 + two hemisphere caps
    // =========================================================

    CoreTypes::MeshData Build_capsule(uint16_t _segments, uint16_t _rings)
    {
        Mesh mesh;

        const uint32_t segments = (_segments < 3) ? 3u : _segments;
        const uint32_t rings = (_rings < 2) ? 2u : _rings;   // per hemisphere

        const float radius = 1.0f;
        const float half_cylinder = 0.5f;   // cylinder spans -0.5..0.5 in Y

        // The capsule is built as a single vertex grid: top hemisphere,
        // then bottom hemisphere, with the cylinder implied by the gap
        // between the two hemisphere equators offset in Y.
        //
        // Latitude index goes 0..(2*rings+1):
        //   0..rings          → top hemisphere (offset +half_cylinder)
        //   rings+1..2*rings+1 → bottom hemisphere (offset -half_cylinder)

        const uint32_t total_lat = 2 * rings + 1;

        for (uint32_t lat = 0; lat <= total_lat; ++lat)
        {
            float y_offset;
            float phi;

            if (lat <= rings)
            {
                // Top hemisphere: phi 0..PI/2
                const float f = static_cast<float>(lat) / static_cast<float>(rings);
                phi = f * (MathLib::Constants::PI * 0.5f);
                y_offset = half_cylinder;
            }
            else
            {
                // Bottom hemisphere: phi PI/2..PI
                const float f = static_cast<float>(lat - rings - 1) / static_cast<float>(rings);
                phi = (MathLib::Constants::PI * 0.5f) + f * (MathLib::Constants::PI * 0.5f);
                y_offset = -half_cylinder;
            }

            const float y_sphere = std::cos(phi) * radius;
            const float r_xz = std::sin(phi) * radius;

            // The hemisphere center is offset so the two caps sit at the
            // ends of the cylinder body.
            const float y = y_sphere + (lat <= rings ? y_offset : y_offset);

            const float v = static_cast<float>(lat) / static_cast<float>(total_lat);

            for (uint32_t s = 0; s <= segments; ++s)
            {
                const float u = static_cast<float>(s) / static_cast<float>(segments);
                const float theta = u * MathLib::Constants::TWO_PI;
                const float x = r_xz * std::cos(theta);
                const float z = r_xz * std::sin(theta);

                // Normal: points away from the nearest hemisphere center.
                const float ny = y_sphere;
                MathLib::Vector3 nrm{ x, ny, z };
                const float nlen = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
                if (nlen > 1e-8f) { nrm.x /= nlen; nrm.y /= nlen; nrm.z /= nlen; }

                mesh.vertices.push_back({ {x, y, z}, nrm, {}, {u, v}, {} });
            }
        }

        const uint32_t stride = segments + 1;
        for (uint32_t lat = 0; lat < total_lat; ++lat)
        {
            for (uint32_t s = 0; s < segments; ++s)
            {
                const uint32_t a = lat * stride + s;
                const uint32_t b = a + stride;

                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(a + 1);

                mesh.indices.push_back(a + 1);
                mesh.indices.push_back(b);
                mesh.indices.push_back(b + 1);
            }
        }

        Finalize(mesh);
        return mesh;
    }

    // =========================================================
    // Dispatch
    // =========================================================

    CoreTypes::MeshData Build(const Primitive_Desc& _desc)
    {
        switch (_desc.type)
        {
        case Primitive_Type::Cube:        return Build_cube();
        case Primitive_Type::Quad:        return Build_quad();
        case Primitive_Type::Triangle:    return Build_triangle();
        case Primitive_Type::Tetrahedron: return Build_tetrahedron();
        case Primitive_Type::Plane:       return Build_plane(_desc.param1);
        case Primitive_Type::Sphere:      return Build_sphere(_desc.param1, _desc.param2);
        case Primitive_Type::Cone:        return Build_cone(_desc.param1);
        case Primitive_Type::Cylinder:    return Build_cylinder(_desc.param1);
        case Primitive_Type::Torus:       return Build_torus(_desc.param1, _desc.param2);
        case Primitive_Type::Capsule:     return Build_capsule(_desc.param1, _desc.param2);
        default:
            throw std::runtime_error("Primitive_Builder: unknown primitive type");
        }
    }

} // namespace ResourceManager::Primitive_Builder