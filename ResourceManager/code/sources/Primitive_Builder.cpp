// UV convention, shared by EVERY primitive built here:
//
//   U grows to the RIGHT as seen from outside the surface.
//   V = 0 is the TOP edge of the image; V grows downward.
//
// This is the Vulkan / glTF convention, and it is the one the rest of the
// engine already assumes: Image_Loader does not flip rows (the first row
// of the file is the top row of the texture) and the shaders sample with
// the vertex UV as is. A generator therefore never picks a V direction on
// its own: it writes its UVs in the "natural" form (U to the right, V UP,
// like a plot) and converts them once, in Image_uv() below.
//
// Two traps this convention exposes in the generators:
//   - Surfaces of revolution run their azimuth from +X toward +Z, and seen
//     from outside that is toward the LEFT, so the natural U is 1 - azimuth
//     fraction (not the azimuth fraction itself).
//   - A disc cap has no intrinsic "up": Cap_uv() fixes it, so the top and
//     bottom caps both stay unmirrored.
//
// Tangents come out of Mesh_Tangents::Compute() from these UVs, with the
// bitangent defined as glTF expects it (see there).

#include <Primitive_Builder.hpp>
#include <Mesh_Tangents.hpp>
#include <MathConstants.hpp>
#include <Vector.hpp>
#include <Vector3.hpp>


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

        // =========================================================
        // UV convention (documented at the top of this file)
        // =========================================================

        // Converts a "natural" UV (U to the right as seen from outside, V UP)
        // to the stored one (V = 0 at the top edge). The ONLY place the V
        // flip happens.
        Vec2 Image_uv(float _u_right, float _v_up)
        {
            return { _u_right, 1.0f - _v_up };
        }

        // UV of the point (_x, _z) of a unit-radius disc cap lying in a
        // horizontal plane, seen from outside. Seen from above (top cap),
        // -Z is the top of the image; seen from below (bottom cap), +Z is.
        // In both cases U grows toward +X, which keeps the mapping
        // unmirrored: flipping only V for both caps would mirror one.
        Vec2 Cap_uv(float _x, float _z, bool _faces_up)
        {
            const float v_up = _faces_up ? (0.5f - 0.5f * _z) : (0.5f + 0.5f * _z);

            return Image_uv(0.5f + 0.5f * _x, v_up);
        }

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
            return MathLib::Vec3::Normalize(MathLib::Vec3::Cross(_p1 - _p0, _p2 - _p0));
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

        // Enforces the two invariants stated in the header: every triangle
        // has a non-zero area and is wound counter-clockwise with respect
        // to its vertex normals. Index ranges are validated as well.
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

            // A triangle is degenerate when its area is negligible compared to
            // its own size, never compared to an absolute number: the area of
            // a legitimate triangle scales with the square of the primitive's
            // size and of its tessellation (a capsule has a quarter of the
            // sphere's radius, so 16 times less area per triangle, and the
            // fan triangles around a pole shrink with the segment count), so
            // any fixed floor rejects valid meshes somewhere in the legal
            // parameter range.
            //
            // The measure is (2 * area) / longest_edge^2: 0 for a collapsed
            // (collinear or coincident) triangle, sqrt(3) / 2 for an
            // equilateral one, scale-invariant, and independent of the vertex
            // order. The thinnest legitimate triangle any generator emits is
            // a pole fan at maximum tessellation, about 2 * pi / 512 = 0.012;
            // the floor sits two orders of magnitude below that, and far above
            // the ~1e-7 that single-precision rounding leaves on a triangle
            // that is collapsed by construction.
            constexpr float min_relative_area = 1e-4f;

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

                const Vec3 cross = MathLib::Vec3::Cross(v1.position - v0.position, v2.position - v0.position);
                const float cross_length_sq = MathLib::Vec3::Length_squared(cross);

                const float longest_edge_sq = std::max({
                    MathLib::Vec3::Length_squared(v1.position - v0.position),
                    MathLib::Vec3::Length_squared(v2.position - v1.position),
                    MathLib::Vec3::Length_squared(v0.position - v2.position) });

                // cross_length = 2 * area, so this is
                // (2 * area / longest_edge^2) <= min_relative_area, squared
                // to avoid the square root. <= also catches the all-zero case.
                if (cross_length_sq <= min_relative_area * min_relative_area * longest_edge_sq * longest_edge_sq)
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

                if (MathLib::Vec3::Dot(cross, vertex_normal_sum) <= 0.0f)
                {
                    throw std::logic_error("Primitive_Builder: " + name + " triangle " +
                        std::to_string(i / 3) + " is wound clockwise relative to its normals");
                }
            }
        }

        // Finalizes a mesh: default color + tangents, then validates the
        // geometry contract.
        void Finalize(Mesh& _mesh, const char* _name)
        {
            Set_default_color(_mesh);
            Mesh_Tangents::Compute(_mesh);

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
        // Corners run counter-clockwise as seen from outside the face:
        // bottom-left, bottom-right, top-right, top-left.
        struct Face { Vec3 normal; Vec3 v[4]; };

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

        // Natural UVs of the corners above: bottom-left, bottom-right,
        // top-right, top-left. On the +Y / -Y faces "top" is -Z / +Z.
        const Vec2 uvs[4] = { Image_uv(0, 0), Image_uv(1, 0), Image_uv(1, 1), Image_uv(0, 1) };

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
    // Quad: hardcoded, 1x1 in XZ plane facing +Y, centered
    // =========================================================

    CoreTypes::MeshData Build_quad()
    {
        Mesh mesh;

        const float h = 0.5f;
        const Vec3 n{ 0, 1, 0 };

        // Counter-clockwise as seen from +Y (looking down). Seen from above
        // the top edge is -Z, the same orientation as the plane.
        Add_vertex(mesh, { -h, 0,  h }, n, Image_uv(0, 0));
        Add_vertex(mesh, {  h, 0,  h }, n, Image_uv(1, 0));
        Add_vertex(mesh, {  h, 0, -h }, n, Image_uv(1, 1));
        Add_vertex(mesh, { -h, 0, -h }, n, Image_uv(0, 1));

        Add_quad(mesh, 0, 1, 2, 3);

        Finalize(mesh, "quad");
        return mesh;
    }

    // =========================================================
    // Triangle: hardcoded, unit triangle in XZ, facing +Y
    // =========================================================

    CoreTypes::MeshData Build_triangle()
    {
        Mesh mesh;

        const Vec3 n{ 0, 1, 0 };

        // Base on the +Z side, apex at -Z: the apex is the top of the image.
        Add_vertex(mesh, { -0.5f, 0,  0.5f }, n, Image_uv(0.0f, 0.0f));
        Add_vertex(mesh, {  0.5f, 0,  0.5f }, n, Image_uv(1.0f, 0.0f));
        Add_vertex(mesh, {  0.0f, 0, -0.5f }, n, Image_uv(0.5f, 1.0f));

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

        // Every face uses the same natural UV triangle. A tetrahedron has no
        // "up" on its faces, so only the handedness matters: the corners are
        // counter-clockwise from outside, which Image_uv keeps unmirrored.
        const Vec2 uvs[3] = { Image_uv(0.0f, 0.0f), Image_uv(1.0f, 0.0f), Image_uv(0.5f, 1.0f) };

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

        // Vertices on a regular grid from -0.5 to 0.5 in X and Z.
        for (uint32_t z = 0; z < verts_per_side; ++z)
        {
            for (uint32_t x = 0; x < verts_per_side; ++x)
            {
                const float fx = static_cast<float>(x) / static_cast<float>(divs);
                const float fz = static_cast<float>(z) / static_cast<float>(divs);

                // fz counts from -Z, the top edge seen from +Y, so the
                // natural height of the point is 1 - fz.
                Add_vertex(mesh, { fx - 0.5f, 0.0f, fz - 0.5f }, n, Image_uv(fx, 1.0f - fz));
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

    // The poles. A pole is a single point, but it cannot be a single VERTEX:
    // its U would have to be one value, and every triangle of the fan that
    // meets it needs a different one (the pole is where all the meridians
    // converge). With one shared U the texture is sheared across the whole
    // fan, at every tessellation. So each fan triangle gets its own copy of
    // the pole, with U at the middle of its segment. That is `segments`
    // vertices per pole, not a ring of segments + 1 coincident ones, and
    // every fan triangle keeps its full area: no zero-area triangle exists.
    // Each copy also gets its own tangent, computed from its one triangle.
    CoreTypes::MeshData Build_sphere(uint16_t _segments, uint16_t _rings)
    {
        Mesh mesh;

        const uint32_t segments = Clamp_param(Primitive_Type::Sphere, 0, _segments);
        const uint32_t rings = Clamp_param(Primitive_Type::Sphere, 1, _rings);
        const uint32_t stride = segments + 1;

        // Rings 1 .. rings-1 (the poles are rings 0 and `rings`).
        for (uint32_t r = 1; r < rings; ++r)
        {
            const float v = static_cast<float>(r) / static_cast<float>(rings);   // 0 at the north pole .. 1 at the south
            const float phi = v * PI;            // 0..PI latitude
            const float y = std::cos(phi);
            const float r_xz = std::sin(phi);

            for (uint32_t s = 0; s <= segments; ++s)
            {
                const float u = static_cast<float>(s) / static_cast<float>(segments);
                const float theta = u * TWO_PI;  // 0..2PI longitude

                const Vec3 p{ r_xz * std::cos(theta), y, r_xz * std::sin(theta) };

                // Unit sphere: position == normal. theta runs from +X toward
                // +Z, i.e. to the left seen from outside, so U to the right
                // is 1 - u; v counts down from the north pole, so the
                // natural height is 1 - v.
                Add_vertex(mesh, p, p, Image_uv(1.0f - u, 1.0f - v));
            }
        }

        // First vertex of ring r (1 <= r <= rings-1).
        const auto ring_start = [&](uint32_t r) { return (r - 1u) * stride; };

        // U (to the right) at the middle of segment s: where its pole copy sits.
        const auto segment_mid_u = [&](uint32_t s) { return 1.0f - (static_cast<float>(s) + 0.5f) / static_cast<float>(segments); };

        // Top fan.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t pole = Add_vertex(mesh, { 0, 1, 0 }, { 0, 1, 0 }, Image_uv(segment_mid_u(s), 1.0f));
            const uint32_t a = ring_start(1) + s;
            Add_triangle(mesh, pole, a + 1, a);
        }

        // Quads between consecutive rings. a is on the upper ring, b right
        // below it on the next ring.
        for (uint32_t r = 1; r + 1 < rings; ++r)
        {
            for (uint32_t s = 0; s < segments; ++s)
            {
                const uint32_t a = ring_start(r) + s;
                const uint32_t b = a + stride;

                Add_triangle(mesh, a, a + 1, b);
                Add_triangle(mesh, a + 1, b + 1, b);
            }
        }

        // Bottom fan.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t pole = Add_vertex(mesh, { 0, -1, 0 }, { 0, -1, 0 }, Image_uv(segment_mid_u(s), 0.0f));
            const uint32_t a = ring_start(rings - 1) + s;
            Add_triangle(mesh, a, a + 1, pole);
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

        // Analytic side normal for base radius 1 and height 1: the surface
        // normal at azimuth t is (cos t, 1, sin t) / sqrt(2).
        const auto side_normal = [](float t)
            {
                return MathLib::Vec3::Normalize(Vec3{ std::cos(t), 1.0f, std::sin(t) });
            };

        const Vec3 apex{ 0.0f, 1.0f, 0.0f };

        // Base ring of the side wall: ONE vertex per azimuth, shared by the
        // two triangles that meet there (the last repeats the first for the
        // U seam). Sharing is what makes the tangent smooth: tangents are
        // accumulated per vertex, so a vertex owned by a single triangle
        // would keep that triangle's flat tangent while its normal is
        // smooth, and the frame would jump at every edge.
        const uint32_t ring = static_cast<uint32_t>(mesh.vertices.size());

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float t = static_cast<float>(s) / seg * TWO_PI;

            // Azimuth runs to the left seen from outside: U to the right
            // is 1 - azimuth fraction. The base is the bottom of the image.
            Add_vertex(mesh, { std::cos(t), 0.0f, std::sin(t) }, side_normal(t),
                Image_uv(1.0f - static_cast<float>(s) / seg, 0.0f));
        }

        // The apex is duplicated per segment: each copy gets the normal and
        // the U of its segment's mid azimuth, as the apex has no single one.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const float t_mid = (static_cast<float>(s) + 0.5f) / seg * TWO_PI;
            const float u_mid = 1.0f - (static_cast<float>(s) + 0.5f) / seg;

            const uint32_t ia = Add_vertex(mesh, apex, side_normal(t_mid), Image_uv(u_mid, 1.0f));

            // base[s] -> apex -> base[s+1] is counter-clockwise from outside.
            Add_triangle(mesh, ring + s, ia, ring + s + 1);
        }

        // Base cap (facing -Y), center fan.
        const Vec3 down{ 0, -1, 0 };
        const uint32_t center = Add_vertex(mesh, { 0, 0, 0 }, down, Cap_uv(0.0f, 0.0f, false));
        const uint32_t rim = static_cast<uint32_t>(mesh.vertices.size());

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float t = static_cast<float>(s) / seg * TWO_PI;
            const float x = std::cos(t);
            const float z = std::sin(t);
            Add_vertex(mesh, { x, 0, z }, down, Cap_uv(x, z, false));
        }

        // Seen from below (-Y), increasing azimuth runs clockwise, so the
        // counter-clockwise order is center -> rim[s] -> rim[s+1].
        for (uint32_t s = 0; s < segments; ++s)
            Add_triangle(mesh, center, rim + s, rim + s + 1);

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

        // Side wall: bottom and top vertex per azimuth, sharing a normal.
        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float u = static_cast<float>(s) / seg;
            const float t = u * TWO_PI;
            const float x = std::cos(t);
            const float z = std::sin(t);
            const Vec3 nrm{ x, 0.0f, z };

            // Azimuth runs to the left seen from outside: U to the right
            // is 1 - u. y = 0 is the bottom, y = 1 the top.
            Add_vertex(mesh, { x, 0.0f, z }, nrm, Image_uv(1.0f - u, 0.0f));
            Add_vertex(mesh, { x, 1.0f, z }, nrm, Image_uv(1.0f - u, 1.0f));
        }

        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t a = s * 2;       // bottom, this azimuth
            const uint32_t b = a + 1;       // top, this azimuth
            const uint32_t c = a + 2;       // bottom, next azimuth
            const uint32_t d = a + 3;       // top, next azimuth

            // Counter-clockwise from outside: bottom -> top -> next bottom.
            Add_triangle(mesh, a, b, c);
            Add_triangle(mesh, b, d, c);
        }

        // Top cap (+Y).
        const Vec3 up{ 0, 1, 0 };
        const uint32_t top_center = Add_vertex(mesh, { 0, 1, 0 }, up, Cap_uv(0.0f, 0.0f, true));
        const uint32_t top_rim = static_cast<uint32_t>(mesh.vertices.size());

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float t = static_cast<float>(s) / seg * TWO_PI;
            const float x = std::cos(t);
            const float z = std::sin(t);
            Add_vertex(mesh, { x, 1, z }, up, Cap_uv(x, z, true));
        }

        // Seen from above (+Y), increasing azimuth runs clockwise, so the
        // counter-clockwise order is center -> rim[s+1] -> rim[s].
        for (uint32_t s = 0; s < segments; ++s)
            Add_triangle(mesh, top_center, top_rim + s + 1, top_rim + s);

        // Bottom cap (-Y).
        const Vec3 down{ 0, -1, 0 };
        const uint32_t bot_center = Add_vertex(mesh, { 0, 0, 0 }, down, Cap_uv(0.0f, 0.0f, false));
        const uint32_t bot_rim = static_cast<uint32_t>(mesh.vertices.size());

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float t = static_cast<float>(s) / seg * TWO_PI;
            const float x = std::cos(t);
            const float z = std::sin(t);
            Add_vertex(mesh, { x, 0, z }, down, Cap_uv(x, z, false));
        }

        for (uint32_t s = 0; s < segments; ++s)
            Add_triangle(mesh, bot_center, bot_rim + s, bot_rim + s + 1);

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

        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float theta = u * TWO_PI;
            const float cos_t = std::cos(theta);
            const float sin_t = std::sin(theta);

            for (uint32_t r = 0; r <= rings; ++r)
            {
                const float v = static_cast<float>(r) / static_cast<float>(rings);
                const float phi = v * TWO_PI;
                const float cos_p = std::cos(phi);
                const float sin_p = std::sin(phi);

                const float x = (main_radius + tube_radius * cos_p) * cos_t;
                const float y = tube_radius * sin_p;
                const float z = (main_radius + tube_radius * cos_p) * sin_t;

                // Normal points outward from the tube center.
                const Vec3 nrm{ cos_p * cos_t, sin_p, cos_p * sin_t };

                // theta runs to the left seen from outside, so U to the right
                // is 1 - u. phi = 0 is the outer equator and grows upward on
                // the visible outer side, so v is already the natural height.
                Add_vertex(mesh, { x, y, z }, nrm, Image_uv(1.0f - u, v));
            }
        }

        const uint32_t stride = rings + 1;
        for (uint32_t s = 0; s < segments; ++s)
        {
            for (uint32_t r = 0; r < rings; ++r)
            {
                const uint32_t a = s * stride + r;   // this azimuth, this tube angle
                const uint32_t b = a + stride;       // next azimuth, this tube angle

                // Counter-clockwise from outside the tube.
                Add_triangle(mesh, a, a + 1, b);
                Add_triangle(mesh, a + 1, b + 1, b);
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
    // Built as one latitude/longitude grid: `rings` rows for the top
    // hemisphere ending at the upper equator, `rings` rows for the bottom
    // hemisphere starting at the lower equator, and the body is the band of
    // quads between the two equators. The poles are one copy per fan
    // triangle with U at the middle of its segment, for the reason given at
    // Build_sphere.
    //
    // V follows the ARC LENGTH along the meridian, not the row index. Rows
    // are evenly spaced in angle on the hemispheres but the body is a single
    // band, so a per-row V would give that band the V span of one hemisphere
    // row (a few percent of the texture for half the capsule's length) and
    // stretch the texture there by about an order of magnitude.
    CoreTypes::MeshData Build_capsule(uint16_t _segments, uint16_t _rings)
    {
        Mesh mesh;

        const uint32_t segments = Clamp_param(Primitive_Type::Capsule, 0, _segments);
        const uint32_t rings = Clamp_param(Primitive_Type::Capsule, 1, _rings);
        const uint32_t stride = segments + 1;

        const float radius = 0.25f;
        const float half_body = 0.25f;

        // Length of the meridian from the north pole to the south pole:
        // two quarter circles and the straight body between them.
        const float cap_length = HALF_PI * radius;
        const float body_length = 2.0f * half_body;
        const float meridian_length = 2.0f * cap_length + body_length;

        const auto add_ring = [&](float phi, float y_offset, float arc_from_north)
            {
                const float y_sphere = std::cos(phi) * radius;
                const float r_xz = std::sin(phi) * radius;
                const float v = arc_from_north / meridian_length;   // 0 at the north pole .. 1 at the south

                for (uint32_t s = 0; s <= segments; ++s)
                {
                    const float u = static_cast<float>(s) / static_cast<float>(segments);
                    const float theta = u * TWO_PI;

                    // Normal: away from the hemisphere center; horizontal on
                    // the equators, which is also the body's normal.
                    const Vec3 nrm{ std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta) };
                    const Vec3 pos{ r_xz * std::cos(theta), y_sphere + y_offset, r_xz * std::sin(theta) };

                    // theta runs to the left seen from outside, so U to the
                    // right is 1 - u; v counts down from the north pole, so
                    // the natural height is 1 - v.
                    Add_vertex(mesh, pos, nrm, Image_uv(1.0f - u, 1.0f - v));
                }
            };

        // Top hemisphere: phi in (0, PI/2], last row is the upper equator.
        for (uint32_t r = 1; r <= rings; ++r)
        {
            const float phi = static_cast<float>(r) / static_cast<float>(rings) * HALF_PI;
            add_ring(phi, half_body, radius * phi);
        }

        // Bottom hemisphere: phi in [PI/2, PI), first row is the lower equator.
        for (uint32_t r = 0; r < rings; ++r)
        {
            const float quarter = static_cast<float>(r) / static_cast<float>(rings) * HALF_PI;
            add_ring(HALF_PI + quarter, -half_body, cap_length + body_length + radius * quarter);
        }

        // First vertex of ring row (1 <= row <= 2*rings).
        const auto row_start = [&](uint32_t row) { return (row - 1u) * stride; };
        const uint32_t last_ring_row = 2 * rings;

        // U (to the right) at the middle of segment s: where its pole copy sits.
        const auto segment_mid_u = [&](uint32_t s) { return 1.0f - (static_cast<float>(s) + 0.5f) / static_cast<float>(segments); };

        // Top fan.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t pole = Add_vertex(mesh, { 0, half_body + radius, 0 }, { 0, 1, 0 }, Image_uv(segment_mid_u(s), 1.0f));
            const uint32_t a = row_start(1) + s;
            Add_triangle(mesh, pole, a + 1, a);
        }

        // Quads between consecutive ring rows (hemispheres and the body).
        for (uint32_t row = 1; row < last_ring_row; ++row)
        {
            for (uint32_t s = 0; s < segments; ++s)
            {
                const uint32_t a = row_start(row) + s;
                const uint32_t b = a + stride;

                Add_triangle(mesh, a, a + 1, b);
                Add_triangle(mesh, a + 1, b + 1, b);
            }
        }

        // Bottom fan.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t pole = Add_vertex(mesh, { 0, -half_body - radius, 0 }, { 0, -1, 0 }, Image_uv(segment_mid_u(s), 0.0f));
            const uint32_t a = row_start(last_ring_row) + s;
            Add_triangle(mesh, a, a + 1, pole);
        }

        Finalize(mesh, "capsule");
        return mesh;
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
