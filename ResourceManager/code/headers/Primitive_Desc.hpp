#pragma once

#include <cstdint>

namespace ResourceManager
{

    // Primitive_Type: the procedural primitives Primitive_Builder can
    // generate. All primitives are UNIT-sized — a sphere has radius 1,
    // a cube spans -0.5..0.5, a cone is 1 tall. Size is applied by the
    // entity's Transform (scale), never baked into the mesh. This lets
    // many differently-sized objects share one GPU mesh.
    enum class Primitive_Type : uint16_t
    {
        // ── No topological parameters (hardcoded geometry) ──
        Cube = 0,
        Quad = 1,
        Triangle = 2,
        Tetrahedron = 3,

        // ── Parametrized by topology (segments / rings / subdivisions) ──
        Plane = 4,   // param1 = subdivisions
        Sphere = 5,   // param1 = segments, param2 = rings
        Cone = 6,   // param1 = segments
        Cylinder = 7,   // param1 = segments
        Torus = 8,   // param1 = segments, param2 = rings
        Capsule = 9,   // param1 = segments, param2 = rings
    };

    // Primitive_Desc: fully describes a primitive to generate.
    //
    // Only TOPOLOGICAL parameters belong here — values that change the
    // number or arrangement of vertices (segments, rings, subdivisions).
    // DIMENSIONAL values (radius, height, width) are NOT here: primitives
    // are unit-sized and scaled via Transform.
    //
    // Parameters not used by a given type are ignored (e.g. Cube uses none).
    // Each parameter is capped at 16 bits (0..65535); callers should keep
    // them well under that — more than a few hundred segments is wasteful.
    struct Primitive_Desc
    {
        Primitive_Type type = Primitive_Type::Cube;
        uint16_t       param1 = 0;   // segments / subdivisions
        uint16_t       param2 = 0;   // rings
        uint16_t       param3 = 0;   // reserved for future primitives

        // =========================================================
        // Key packing
        // =========================================================

        // Packs this descriptor into a unique uint64_t cache key.
        //
        // Layout (low → high bits):
        //   bits [0  - 15]  type
        //   bits [16 - 31]  param1
        //   bits [32 - 47]  param2
        //   bits [48 - 62]  param3 (15 bits — bit 63 reserved)
        //   bit  [63]       discriminator = 1 (marks this as a primitive key)
        //
        // The discriminator bit guarantees primitive keys never collide
        // with file-asset keys (which are FNV64 hashes with bit 63 = 0).
        // Because every distinct parameter combination maps to a distinct
        // integer, primitive keys are collision-free by construction —
        // no hashing involved.
        uint64_t To_key() const
        {
            constexpr uint64_t PRIMITIVE_DISCRIMINATOR = (uint64_t(1) << 63);

            return PRIMITIVE_DISCRIMINATOR
                | static_cast<uint64_t>(type)
                | (static_cast<uint64_t>(param1) << 16)
                | (static_cast<uint64_t>(param2) << 32)
                | (static_cast<uint64_t>(param3) << 48 & 0x7FFF000000000000ull);
        }
    };

} // namespace ResourceManager