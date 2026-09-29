#include <Primitive_Builder.hpp>
#include <MathConstants.hpp>
#include <Vector.hpp>
#include <Vector3.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace ResourceManager::Primitive_Builder
{

    namespace
    {
        using Vertex = CoreTypes::Vertex_Static_Mesh_CPU;
        using Mesh = CoreTypes::MeshData;
        using Vec2 = MathLib::Vector2;
        using Vec3 = MathLib::Vector3;

        constexpr float PI = MathLib::Constants::PI;
        constexpr float TWO_PI = MathLib::Constants::TWO_PI;
        constexpr float HALF_PI = MathLib::Constants::HALF_PI;

        uint32_t Clamp_param(Primitive_Type _type, uint8_t _index, uint16_t _value)
        {
            const Param_Range range = Spec_of(_type).range[_index];

            if (_value < range.min) return range.min;
            if (_value > range.max) return range.max;

            return _value;
        }

        // =========================================================
        // Mesh assembly helpers
        // =========================================================

        // Appends a vertex and returns its index. Tangent and color are
        // filled in by Finalize().
        uint32_t Add_vertex(Mesh& _mesh, const Vec3& _position, const Vec3& _normal, const Vec2& _uv)
        {
            Vertex vertex{};
            vertex.position = _position;
            vertex.normal = _normal;
            vertex.uv = _uv;

            _mesh.vertices.push_back(vertex);

            return static_cast<uint32_t>(_mesh.vertices.size() - 1);
        }

        // Appends one triangle. The three indices must be given in
        // counter-clockwise order as seen from outside the surface.
        void Add_triangle(Mesh& _mesh, uint32_t _a, uint32_t _b, uint32_t _c)
        {
            _mesh.indices.push_back(_a);
            _mesh.indices.push_back(_b);
            _mesh.indices.push_back(_c);
        }

        // Appends the two triangles of a quad whose corners a, b, c, d run
        // counter-clockwise as seen from outside.
        void Add_quad(Mesh& _mesh, uint32_t _a, uint32_t _b, uint32_t _c, uint32_t _d)
        {
            Add_triangle(_mesh, _a, _b, _c);
            Add_triangle(_mesh, _a, _c, _d);
        }

        // Unit normal of a counter-clockwise triangle: points to the side
        // from which the winding appears counter-clockwise.
        Vec3 Face_normal(const Vec3& _p0, const Vec3& _p1, const Vec3& _p2)
        {
            return MathLib::Vec3::Normalize(glm::cross(_p1 - _p0, _p2 - _p0));
        }

        // =========================================================
        // Surfaces of revolution
        // =========================================================

        // Azimuth of the texture coordinate _u of a surface of revolution
        // around +Y (see "Surfaces of revolution" in Primitive_Builder.hpp):
        //   theta = PI/2 + 2*PI*u
        // u = 0 and u = 1 (the seam) lie on -Z, behind the object as seen
        // by the default camera, and u = 0.5 faces +Z.
        float Azimuth_of_u(float _u)
        {
            return HALF_PI + _u * TWO_PI;
        }

        // Unit horizontal direction at azimuth _theta:
        //   (cos(theta), 0, -sin(theta))
        // theta grows counter-clockwise seen from +Y, so seen from outside
        // the surface it advances to the right: u grows to the right, as
        // the UV convention requires.
        Vec3 Radial_direction(float _theta)
        {
            return { std::cos(_theta), 0.0f, -std::sin(_theta) };
        }

        // =========================================================
        // Tangent computation
        // =========================================================

        // Computes per-vertex tangents from positions, UVs and normals
        // using the standard Lengyel method (gradient of UV over the
        // triangle). Fills vertex.tangent.xyz with the orthonormalized
        // tangent and vertex.tangent.w with the bitangent sign (+1/-1),
        // with the glTF convention: the bitangent cross(N, T) * w points
        // towards DECREASING v, i.e. up in the image (+Y of a tangent
        // space normal map). A surface mapped without a mirror under the
        // UV convention of Primitive_Builder.hpp therefore gets w = +1,
        // exactly like an unmirrored glTF mesh, and the shaders can treat
        // both sources the same way.
        //
        // Call after all positions, normals, UVs and indices are set.
        void Compute_tangents(Mesh& _mesh)
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

                const Vertex& v0 = _mesh.vertices[i0];
                const Vertex& v1 = _mesh.vertices[i1];
                const Vertex& v2 = _mesh.vertices[i2];

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
                const Vec3 bitangent = f * (d1.x * e2 - d2.x * e1);

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
                Vec3 tangent = tan_accum[i] - n * glm::dot(n, tan_accum[i]);

                // NOT a bare normalize: a zero-length tangent (a vertex whose
                // triangles are all degenerate in UV) would normalize to NaN,
                // and a NaN here reaches the vertex buffer and lights the
                // surface with garbage. The arbitrary fallback stays finite.
                const float len = glm::length(tangent);
                tangent = (len > 1e-8f) ? tangent / len : Vec3(1.0f, 0.0f, 0.0f);

                // The bitangent is not stored: the shader rebuilds it as
                // cross(N, T) * w. w is that reconstruction's sign, measured
                // against -dP/dv (towards decreasing v, the glTF direction).
                const float sign =
                    (glm::dot(glm::cross(n, tangent), -bitan_accum[i]) < 0.0f) ? -1.0f : 1.0f;

                _mesh.vertices[i].tangent = MathLib::Vector4(tangent, sign);
            }
        }

        // Sets every vertex color to white (default tint).
        void Set_default_color(Mesh& _mesh)
        {
            for (Vertex& v : _mesh.vertices)
                v.color = { 1.0f, 1.0f, 1.0f, 1.0f };
        }

        // =========================================================
        // Geometry validation
        // =========================================================

        // Enforces the invariants stated in the header: every triangle has
        // a non-zero area, is wound counter-clockwise with respect to its
        // vertex normals, and maps the texture without a mirror. Index
        // ranges are validated as well.
        //
        // Runs for every generated mesh, in every build. Generation is a
        // load-time operation, so the O(triangles) cost is negligible,
        // and a violation is a generator bug that must be visible in a
        // release build too: rendering it would show a missing or
        // inside-out object with no error anywhere.
        void Validate_geometry(const Mesh& _mesh, const char* _name)
        {
            const std::string name = _name;

            if (_mesh.vertices.empty() || _mesh.indices.empty())
                throw std::logic_error("Primitive_Builder: " + name + " produced an empty mesh");

            if (_mesh.indices.size() % 3 != 0)
                throw std::logic_error("Primitive_Builder: " + name + " index count is not a multiple of 3");

            const size_t vertex_count = _mesh.vertices.size();

            // Squared length of the edge cross product, i.e. (2 * area)^2.
            // Unit-sized primitives at the maximum tessellation (512 x 512)
            // still produce triangles far above this threshold.
            constexpr float min_cross_length_sq = 1e-14f;

            // Twice the smallest UV area considered oriented. The smallest
            // UV cell at the maximum tessellation is (1/512)^2 ~ 3.8e-6.
            constexpr float min_uv_area = 1e-10f;

            for (size_t i = 0; i < _mesh.indices.size(); i += 3)
            {
                const uint32_t i0 = _mesh.indices[i + 0];
                const uint32_t i1 = _mesh.indices[i + 1];
                const uint32_t i2 = _mesh.indices[i + 2];

                if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
                {
                    throw std::logic_error("Primitive_Builder: " + name + " triangle " +
                        std::to_string(i / 3) + " references a vertex out of range");
                }

                const Vertex& v0 = _mesh.vertices[i0];
                const Vertex& v1 = _mesh.vertices[i1];
                const Vertex& v2 = _mesh.vertices[i2];

                const Vec3 cross = glm::cross(v1.position - v0.position, v2.position - v0.position);
                const float cross_length_sq = glm::dot(cross, cross);

                if (cross_length_sq < min_cross_length_sq)
                {
                    throw std::logic_error("Primitive_Builder: " + name + " triangle " +
                        std::to_string(i / 3) + " is degenerate (zero area)");
                }

                // The geometric normal must agree with the vertex normals.
                // The three are summed instead of tested one by one so a
                // coarse tessellation (a 3-segment sphere, a cone apex) is
                // not rejected for the honest angle between a flat face and
                // a smooth normal.
                const Vec3 vertex_normal_sum = v0.normal + v1.normal + v2.normal;

                if (glm::dot(cross, vertex_normal_sum) <= 0.0f)
                {
                    throw std::logic_error("Primitive_Builder: " + name + " triangle " +
                        std::to_string(i / 3) + " is wound clockwise relative to its normals");
                }

                // UV orientation. The triangle is counter-clockwise seen
                // from outside (checked above). With u to the right and v
                // downwards, an unmirrored mapping runs clockwise in the
                // (u, v) axes, so the signed UV area is negative; a
                // positive one is a mirrored image (a horizontal or a
                // vertical flip). Triangles degenerate in UV (collinear
                // coordinates) carry no orientation and are skipped.
                //
                // A 180 degree rotation keeps the sign and is not detected
                // here: the written convention and the known-UV checks of
                // each generator cover it.
                //
                // Primitives only: a loaded mesh may be mirrored on purpose
                // (a mirrored half of a symmetric model).
                const Vec2  d1 = v1.uv - v0.uv;
                const Vec2  d2 = v2.uv - v0.uv;
                const float uv_area = d1.x * d2.y - d2.x * d1.y;

                if (uv_area > min_uv_area)
                {
                    throw std::logic_error("Primitive_Builder: " + name + " triangle " +
                        std::to_string(i / 3) + " maps the texture mirrored (UV convention violated)");
                }
            }
        }

        // Finalizes a mesh: default color + tangents + UINT32 indices, then
        // validates the geometry contract.
        void Finalize(Mesh& _mesh, const char* _name)
        {
            Set_default_color(_mesh);
            Compute_tangents(_mesh);
            _mesh.index_type = CoreTypes::Index_Type::UINT32;

            Validate_geometry(_mesh, _name);
        }

    } // anonymous namespace

    // =========================================================
    // Cube: hardcoded, 24 vertices (4 per face for per-face normals)
    // =========================================================

    CoreTypes::MeshData Build_cube()
    {
        Mesh mesh;

        // Each face: 4 vertices with the same normal, 2 triangles.
        // Corners run counter-clockwise as seen from outside the face, in
        // the face's canonical orientation (Primitive_Builder.hpp):
        // bottom-left, bottom-right, top-right, top-left.
        struct Face { Vec3 normal; Vec3 v[4]; };

        const float h = 0.5f;
        const Face faces[6] = {
            // +X (image up = +Y, right = -Z)
            { { 1, 0, 0}, {{ h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h}} },
            // -X (image up = +Y, right = +Z)
            { {-1, 0, 0}, {{-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h}} },
            // +Y (image up = -Z, right = +X)
            { { 0, 1, 0}, {{-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h}} },
            // -Y (image up = +Z, right = +X)
            { { 0,-1, 0}, {{-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h}} },
            // +Z (image up = +Y, right = +X)
            { { 0, 0, 1}, {{-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h}} },
            // -Z (image up = +Y, right = -X)
            { { 0, 0,-1}, {{ h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h}} },
        };

        // Top-left origin: the bottom corners take v = 1, the top ones v = 0.
        const Vec2 uvs[4] = { {0,1}, {1,1}, {1,0}, {0,0} };

        for (const Face& face : faces)
        {
            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());

            for (int i = 0; i < 4; ++i)
                Add_vertex(mesh, face.v[i], face.normal, uvs[i]);

            Add_quad(mesh, base + 0, base + 1, base + 2, base + 3);
        }

        Finalize(mesh, "cube");
        return mesh;
    }

    // =========================================================
    // Quad: alias of Plane(1)
    // =========================================================

    // Same vertices, UVs and diagonal as Build_plane(1): the two types
    // describe one mesh, and Primitive_Desc::Canonical() maps Quad to
    // Plane(1) so the cache holds it once.
    CoreTypes::MeshData Build_quad()
    {
        return Build_plane(1);
    }

    // =========================================================
    // Triangle: hardcoded, unit triangle in XZ, facing +Y
    // =========================================================

    CoreTypes::MeshData Build_triangle()
    {
        Mesh mesh;

        const Vec3 n{ 0, 1, 0 };

        // Seen from +Y with image up = -Z: the base (z = +0.5) is the bottom
        // of the image (v = 1) and the apex (z = -0.5) its top (v = 0).
        Add_vertex(mesh, { -0.5f, 0,  0.5f }, n, { 0.0f, 1.0f });
        Add_vertex(mesh, {  0.5f, 0,  0.5f }, n, { 1.0f, 1.0f });
        Add_vertex(mesh, {  0.0f, 0, -0.5f }, n, { 0.5f, 0.0f });

        Add_triangle(mesh, 0, 1, 2);

        Finalize(mesh, "triangle");
        return mesh;
    }

    // =========================================================
    // Tetrahedron: hardcoded, 4 triangular faces, flat-shaded
    // =========================================================

    CoreTypes::MeshData Build_tetrahedron()
    {
        Mesh mesh;

        // Four corners of a regular tetrahedron of edge length 1, centered
        // at the origin (circumradius sqrt(6)/4).
        const Vec3 a{  0.0f,     0.6124f,  0.0f    };
        const Vec3 b{  0.0f,    -0.2041f,  0.5774f };
        const Vec3 c{ -0.5000f, -0.2041f, -0.2887f };
        const Vec3 d{  0.5000f, -0.2041f, -0.2887f };

        // Each face is its own 3 vertices for flat normals. Corners are
        // listed counter-clockwise as seen from outside, so Face_normal()
        // points outward.
        const Vec3 tris[4][3] = {
            { a, c, b },
            { a, d, c },
            { a, b, d },
            { b, c, d },
        };

        // The same triangle of the image on every face, counter-clockwise
        // in the image (bottom-left, bottom-right, top-center) like the
        // corners above, so no face is mirrored.
        const Vec2 uvs[3] = { {0.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.0f} };

        for (const auto& tri : tris)
        {
            const Vec3 normal = Face_normal(tri[0], tri[1], tri[2]);

            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            for (int i = 0; i < 3; ++i)
                Add_vertex(mesh, tri[i], normal, uvs[i]);

            Add_triangle(mesh, base + 0, base + 1, base + 2);
        }

        Finalize(mesh, "tetrahedron");
        return mesh;
    }

    // =========================================================
    // Plane: 1x1 grid in XZ subdivided _subdivisions times, facing +Y
    // =========================================================

    CoreTypes::MeshData Build_plane(uint16_t _subdivisions)
    {
        Mesh mesh;

        const uint32_t divs = Clamp_param(Primitive_Type::Plane, 0, _subdivisions);
        const uint32_t verts_per_side = divs + 1;
        const Vec3 n{ 0, 1, 0 };

        // Vertices on a regular grid from -0.5 to 0.5 in X and Z. Seen from
        // +Y with image up = -Z: u grows with X (right) and v with Z (down).
        for (uint32_t z = 0; z < verts_per_side; ++z)
        {
            for (uint32_t x = 0; x < verts_per_side; ++x)
            {
                const float fx = static_cast<float>(x) / static_cast<float>(divs);
                const float fz = static_cast<float>(z) / static_cast<float>(divs);

                Add_vertex(mesh, { fx - 0.5f, 0.0f, fz - 0.5f }, n, { fx, fz });
            }
        }

        // Two triangles per grid cell, counter-clockwise as seen from +Y.
        for (uint32_t z = 0; z < divs; ++z)
        {
            for (uint32_t x = 0; x < divs; ++x)
            {
                const uint32_t tl = z * verts_per_side + x;
                const uint32_t tr = tl + 1;
                const uint32_t bl = tl + verts_per_side;
                const uint32_t br = bl + 1;

                Add_triangle(mesh, tl, bl, br);
                Add_triangle(mesh, tl, br, tr);
            }
        }

        Finalize(mesh, "plane");
        return mesh;
    }

    // =========================================================
    // Sphere: UV sphere, radius 1, centered
    // =========================================================

    // Each pole is a fan of `segments` vertices at the same position, one
    // per segment, each with u at the middle of its segment. A single
    // shared pole vertex would need one fixed UV for every segment, which
    // twists the top and bottom bands of the texture into a fan, and its
    // tangent would be the sum of the tangents of the whole fan (a ring
    // of chords that cancels out to rounding noise).
    //
    // Coincident vertices are only harmful when they are joined by quads:
    // a ring of pole vertices bridged to the first ring with quads yields
    // one zero-area triangle per segment. Here each pole vertex belongs to
    // exactly one triangle of the fan, whose other two vertices are on
    // the first ring, so no triangle is degenerate. Each pole vertex's
    // tangent is the direction of increasing azimuth at its segment and
    // its w matches the adjacent ring.
    //
    // The texture is still sampled over a triangle instead of a rectangle
    // in each polar cell, the residual distortion of any latitude and
    // longitude mesh; it shrinks as `rings` grows.
    CoreTypes::MeshData Build_sphere(uint16_t _segments, uint16_t _rings)
    {
        Mesh mesh;

        const uint32_t segments = Clamp_param(Primitive_Type::Sphere, 0, _segments);
        const uint32_t rings = Clamp_param(Primitive_Type::Sphere, 1, _rings);
        const uint32_t stride = segments + 1;
        const float    seg = static_cast<float>(segments);

        // North pole fan: one vertex per segment, v = 0.
        const uint32_t north_pole = static_cast<uint32_t>(mesh.vertices.size());
        for (uint32_t s = 0; s < segments; ++s)
            Add_vertex(mesh, { 0, 1, 0 }, { 0, 1, 0 }, { (static_cast<float>(s) + 0.5f) / seg, 0.0f });

        // Rings 1 .. rings-1 (the poles are rings 0 and `rings`). Each ring
        // repeats its first vertex at u = 1 to close the seam.
        const uint32_t first_ring = static_cast<uint32_t>(mesh.vertices.size());
        for (uint32_t r = 1; r < rings; ++r)
        {
            const float v = static_cast<float>(r) / static_cast<float>(rings);
            const float phi = v * PI;            // 0..PI polar angle from +Y
            const float y = std::cos(phi);
            const float r_xz = std::sin(phi);

            for (uint32_t s = 0; s <= segments; ++s)
            {
                const float u = static_cast<float>(s) / seg;
                const Vec3  p = Radial_direction(Azimuth_of_u(u)) * r_xz + Vec3{ 0.0f, y, 0.0f };

                Add_vertex(mesh, p, p, { u, v });   // unit sphere: position == normal
            }
        }

        // South pole fan: one vertex per segment, v = 1.
        const uint32_t south_pole = static_cast<uint32_t>(mesh.vertices.size());
        for (uint32_t s = 0; s < segments; ++s)
            Add_vertex(mesh, { 0, -1, 0 }, { 0, -1, 0 }, { (static_cast<float>(s) + 0.5f) / seg, 1.0f });

        // First vertex of ring r (1 <= r <= rings-1).
        const auto ring_start = [&](uint32_t r) { return first_ring + (r - 1u) * stride; };

        // Seen from outside, u grows to the right and v downwards, so
        // vertex a of a ring is the top-left corner of its cell, a + 1 the
        // top-right one and a + stride (next ring) the bottom-left one.

        // Top fan: pole (top) -> bottom-left -> bottom-right.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t a = ring_start(1) + s;
            Add_triangle(mesh, north_pole + s, a, a + 1);
        }

        // Quads between consecutive rings.
        for (uint32_t r = 1; r + 1 < rings; ++r)
        {
            for (uint32_t s = 0; s < segments; ++s)
            {
                const uint32_t a = ring_start(r) + s;
                const uint32_t b = a + stride;

                Add_triangle(mesh, a, b, b + 1);
                Add_triangle(mesh, a, b + 1, a + 1);
            }
        }

        // Bottom fan: top-left -> pole (bottom) -> top-right.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t a = ring_start(rings - 1) + s;
            Add_triangle(mesh, a, south_pole + s, a + 1);
        }

        Finalize(mesh, "sphere");
        return mesh;
    }

    // =========================================================
    // Cone: radius 1 base at y=0, apex at y=1, _segments around
    // =========================================================

    CoreTypes::MeshData Build_cone(uint16_t _segments)
    {
        Mesh mesh;

        const uint32_t segments = Clamp_param(Primitive_Type::Cone, 0, _segments);
        const float    seg = static_cast<float>(segments);

        // Analytic side normal for base radius 1 and height 1: at radial
        // direction R the surface normal is (R + (0, 1, 0)) / sqrt(2).
        const auto side_normal = [](float t)
            {
                return MathLib::Vec3::Normalize(Radial_direction(t) + Vec3{ 0.0f, 1.0f, 0.0f });
            };

        const Vec3 apex{ 0.0f, 1.0f, 0.0f };

        // Side: the apex is duplicated per segment so each triangle gets its
        // own apex normal (the normal at the segment's mid azimuth) and its
        // own u. The base is the bottom of the image (v = 1), the apex its
        // top (v = 0).
        for (uint32_t s = 0; s < segments; ++s)
        {
            const float u0 = static_cast<float>(s) / seg;
            const float u1 = static_cast<float>(s + 1) / seg;
            const float um = (static_cast<float>(s) + 0.5f) / seg;

            const float t0 = Azimuth_of_u(u0);
            const float t1 = Azimuth_of_u(u1);

            const uint32_t i0 = Add_vertex(mesh, Radial_direction(t0), side_normal(t0), { u0, 1.0f });
            const uint32_t i1 = Add_vertex(mesh, Radial_direction(t1), side_normal(t1), { u1, 1.0f });
            const uint32_t ia = Add_vertex(mesh, apex, side_normal(Azimuth_of_u(um)), { um, 0.0f });

            // bottom-left -> bottom-right -> apex, counter-clockwise from outside.
            Add_triangle(mesh, i0, i1, ia);
        }

        // Base cap (facing -Y), center fan. Seen from below with image
        // up = +Z: u grows with X and v decreases with Z.
        const Vec3 down{ 0, -1, 0 };
        const uint32_t center = Add_vertex(mesh, { 0, 0, 0 }, down, { 0.5f, 0.5f });
        const uint32_t rim = static_cast<uint32_t>(mesh.vertices.size());

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const Vec3 p = Radial_direction(Azimuth_of_u(static_cast<float>(s) / seg));
            Add_vertex(mesh, p, down, { 0.5f + p.x * 0.5f, 0.5f - p.z * 0.5f });
        }

        // Seen from below (-Y), increasing azimuth runs clockwise, so the
        // counter-clockwise order is center -> rim[s+1] -> rim[s].
        for (uint32_t s = 0; s < segments; ++s)
            Add_triangle(mesh, center, rim + s + 1, rim + s);

        Finalize(mesh, "cone");
        return mesh;
    }

    // =========================================================
    // Cylinder: radius 1, y from 0 to 1, _segments around
    // =========================================================

    CoreTypes::MeshData Build_cylinder(uint16_t _segments)
    {
        Mesh mesh;

        const uint32_t segments = Clamp_param(Primitive_Type::Cylinder, 0, _segments);
        const float    seg = static_cast<float>(segments);

        // Side wall: bottom (v = 1) and top (v = 0) vertex per azimuth,
        // sharing a normal.
        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float u = static_cast<float>(s) / seg;
            const Vec3  radial = Radial_direction(Azimuth_of_u(u));

            Add_vertex(mesh, radial, radial, { u, 1.0f });
            Add_vertex(mesh, radial + Vec3{ 0.0f, 1.0f, 0.0f }, radial, { u, 0.0f });
        }

        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t a = s * 2;       // bottom, this azimuth  (bottom-left)
            const uint32_t b = a + 1;       // top, this azimuth     (top-left)
            const uint32_t c = a + 2;       // bottom, next azimuth  (bottom-right)
            const uint32_t d = a + 3;       // top, next azimuth     (top-right)

            // Counter-clockwise from outside.
            Add_triangle(mesh, b, a, c);
            Add_triangle(mesh, b, c, d);
        }

        // Top cap (+Y). Seen from above with image up = -Z: u grows with X
        // and v with Z.
        const Vec3 up{ 0, 1, 0 };
        const uint32_t top_center = Add_vertex(mesh, { 0, 1, 0 }, up, { 0.5f, 0.5f });
        const uint32_t top_rim = static_cast<uint32_t>(mesh.vertices.size());

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const Vec3 p = Radial_direction(Azimuth_of_u(static_cast<float>(s) / seg));
            Add_vertex(mesh, p + up, up, { 0.5f + p.x * 0.5f, 0.5f + p.z * 0.5f });
        }

        // Seen from above (+Y), increasing azimuth runs counter-clockwise,
        // so the order is center -> rim[s] -> rim[s+1].
        for (uint32_t s = 0; s < segments; ++s)
            Add_triangle(mesh, top_center, top_rim + s, top_rim + s + 1);

        // Bottom cap (-Y). Seen from below with image up = +Z: u grows with
        // X and v decreases with Z.
        const Vec3 down{ 0, -1, 0 };
        const uint32_t bot_center = Add_vertex(mesh, { 0, 0, 0 }, down, { 0.5f, 0.5f });
        const uint32_t bot_rim = static_cast<uint32_t>(mesh.vertices.size());

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const Vec3 p = Radial_direction(Azimuth_of_u(static_cast<float>(s) / seg));
            Add_vertex(mesh, p, down, { 0.5f + p.x * 0.5f, 0.5f - p.z * 0.5f });
        }

        // Seen from below (-Y), increasing azimuth runs clockwise, so the
        // counter-clockwise order is center -> rim[s+1] -> rim[s].
        for (uint32_t s = 0; s < segments; ++s)
            Add_triangle(mesh, bot_center, bot_rim + s + 1, bot_rim + s);

        Finalize(mesh, "cylinder");
        return mesh;
    }

    // =========================================================
    // Torus: outer radius 1, tube radius 0.25
    // =========================================================

    CoreTypes::MeshData Build_torus(uint16_t _segments, uint16_t _rings)
    {
        Mesh mesh;

        const uint32_t segments = Clamp_param(Primitive_Type::Torus, 0, _segments);  // around the main ring
        const uint32_t rings = Clamp_param(Primitive_Type::Torus, 1, _rings);        // around the tube

        const float main_radius = 0.75f;   // so outer radius = 1.0
        const float tube_radius = 0.25f;

        // u runs around the main ring like every surface of revolution.
        // v runs around the tube starting at the outer equator (v = 0) and
        // going DOWN the outer face first, so on the outer face, the one
        // seen from outside, v grows downwards as the convention requires.
        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const Vec3  radial = Radial_direction(Azimuth_of_u(u));

            for (uint32_t r = 0; r <= rings; ++r)
            {
                const float v = static_cast<float>(r) / static_cast<float>(rings);
                const float phi = v * TWO_PI;
                const float cos_p = std::cos(phi);
                const float sin_p = std::sin(phi);

                // Normal points outward from the tube center.
                const Vec3 nrm = radial * cos_p + Vec3{ 0.0f, -sin_p, 0.0f };
                const Vec3 pos = radial * main_radius + nrm * tube_radius;

                Add_vertex(mesh, pos, nrm, { u, v });
            }
        }

        const uint32_t stride = rings + 1;
        for (uint32_t s = 0; s < segments; ++s)
        {
            for (uint32_t r = 0; r < rings; ++r)
            {
                const uint32_t a = s * stride + r;   // top-left: this azimuth, this tube angle
                const uint32_t b = a + stride;       // top-right: next azimuth

                // a + 1 is bottom-left, b + 1 bottom-right. Counter-clockwise
                // from outside the tube.
                Add_triangle(mesh, a, a + 1, b + 1);
                Add_triangle(mesh, a, b + 1, b);
            }
        }

        Finalize(mesh, "torus");
        return mesh;
    }

    // =========================================================
    // Capsule: radius 0.25, total height 1 (y from -0.5 to 0.5)
    // =========================================================

    // Unit-sized like every other primitive: the body spans -0.25..0.25 in
    // Y and each hemispherical cap adds 0.25, for a total height of 1.
    // Built as one latitude/longitude grid: a north pole fan (one vertex
    // per segment, as in Build_sphere), `rings` rows for the top
    // hemisphere ending at the upper equator, `rings` rows for the bottom
    // hemisphere starting at the lower equator, and a south pole fan. The
    // body is the band of quads between the two equators.
    CoreTypes::MeshData Build_capsule(uint16_t _segments, uint16_t _rings)
    {
        Mesh mesh;

        const uint32_t segments = Clamp_param(Primitive_Type::Capsule, 0, _segments);
        const uint32_t rings = Clamp_param(Primitive_Type::Capsule, 1, _rings);
        const uint32_t stride = segments + 1;
        const float    seg = static_cast<float>(segments);

        const float radius = 0.25f;
        const float half_body = 0.25f;

        // Rows 0 and 2*rings+1 are the poles; rows 1..2*rings are rings.
        const uint32_t row_count = 2 * rings + 2;
        const float    v_scale = 1.0f / static_cast<float>(row_count - 1);

        const uint32_t north_pole = static_cast<uint32_t>(mesh.vertices.size());
        for (uint32_t s = 0; s < segments; ++s)
            Add_vertex(mesh, { 0, half_body + radius, 0 }, { 0, 1, 0 }, { (static_cast<float>(s) + 0.5f) / seg, 0.0f });

        const uint32_t first_ring = static_cast<uint32_t>(mesh.vertices.size());

        const auto add_ring = [&](float phi, float y_offset, uint32_t row)
            {
                const float v = static_cast<float>(row) * v_scale;

                for (uint32_t s = 0; s <= segments; ++s)
                {
                    const float u = static_cast<float>(s) / seg;
                    const Vec3  radial = Radial_direction(Azimuth_of_u(u));

                    // Normal: away from the hemisphere center; horizontal on
                    // the equators, which is also the body's normal.
                    const Vec3 nrm = radial * std::sin(phi) + Vec3{ 0.0f, std::cos(phi), 0.0f };
                    const Vec3 pos = nrm * radius + Vec3{ 0.0f, y_offset, 0.0f };

                    Add_vertex(mesh, pos, nrm, { u, v });
                }
            };

        // Top hemisphere: phi in (0, PI/2], last row is the upper equator.
        for (uint32_t r = 1; r <= rings; ++r)
            add_ring(static_cast<float>(r) / static_cast<float>(rings) * HALF_PI, half_body, r);

        // Bottom hemisphere: phi in [PI/2, PI), first row is the lower equator.
        for (uint32_t r = 0; r < rings; ++r)
            add_ring(HALF_PI + static_cast<float>(r) / static_cast<float>(rings) * HALF_PI, -half_body, rings + 1 + r);

        const uint32_t south_pole = static_cast<uint32_t>(mesh.vertices.size());
        for (uint32_t s = 0; s < segments; ++s)
            Add_vertex(mesh, { 0, -half_body - radius, 0 }, { 0, -1, 0 }, { (static_cast<float>(s) + 0.5f) / seg, 1.0f });

        // First vertex of ring row (1 <= row <= 2*rings).
        const auto row_start = [&](uint32_t row) { return first_ring + (row - 1u) * stride; };
        const uint32_t last_ring_row = 2 * rings;

        // Same cell layout and windings as Build_sphere.

        // Top fan.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t a = row_start(1) + s;
            Add_triangle(mesh, north_pole + s, a, a + 1);
        }

        // Quads between consecutive ring rows (hemispheres and the body).
        for (uint32_t row = 1; row < last_ring_row; ++row)
        {
            for (uint32_t s = 0; s < segments; ++s)
            {
                const uint32_t a = row_start(row) + s;
                const uint32_t b = a + stride;

                Add_triangle(mesh, a, b, b + 1);
                Add_triangle(mesh, a, b + 1, a + 1);
            }
        }

        // Bottom fan.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t a = row_start(last_ring_row) + s;
            Add_triangle(mesh, a, south_pole + s, a + 1);
        }

        Finalize(mesh, "capsule");
        return mesh;
    }

    // =========================================================
    // Direction -> sphere UV
    // =========================================================

    MathLib::Vector2 Sphere_uv_of_direction(const MathLib::Vector3& _direction)
    {
        const Vec3 d = MathLib::Vec3::Normalize(_direction);

        // Inverse of Azimuth_of_u / Radial_direction: theta = atan2(-z, x),
        // then u = (theta - PI/2) / (2*PI) wrapped into [0, 1).
        const float theta = std::atan2(-d.z, d.x);
        float       u = (theta - HALF_PI) / TWO_PI;
        u -= std::floor(u);

        const float v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / PI;

        return { u, v };
    }

    // =========================================================
    // Dispatch
    // =========================================================

    CoreTypes::MeshData Build(const Primitive_Desc& _desc)
    {
        // The canonical descriptor is what the cache keys on, so it is also
        // what gets built: dispatching on the raw fields would let a
        // non-canonical value produce geometry the key does not describe.
        const Primitive_Desc desc = _desc.Canonical();

        switch (desc.type)
        {
        case Primitive_Type::Cube:        return Build_cube();
        case Primitive_Type::Quad:        return Build_quad();
        case Primitive_Type::Triangle:    return Build_triangle();
        case Primitive_Type::Tetrahedron: return Build_tetrahedron();
        case Primitive_Type::Plane:       return Build_plane(desc.param1);
        case Primitive_Type::Sphere:      return Build_sphere(desc.param1, desc.param2);
        case Primitive_Type::Cone:        return Build_cone(desc.param1);
        case Primitive_Type::Cylinder:    return Build_cylinder(desc.param1);
        case Primitive_Type::Torus:       return Build_torus(desc.param1, desc.param2);
        case Primitive_Type::Capsule:     return Build_capsule(desc.param1, desc.param2);
        }

        throw std::invalid_argument("Primitive_Builder: unknown primitive type " +
            std::to_string(static_cast<unsigned>(desc.type)));
    }

} // namespace ResourceManager::Primitive_Builder
