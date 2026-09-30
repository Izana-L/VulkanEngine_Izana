#pragma once

#include <cassert>
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

    // Last valid enumerator — update when a primitive is added.
    inline constexpr Primitive_Type PRIMITIVE_TYPE_LAST = Primitive_Type::Capsule;

    // True if _type is one of the enumerators above. Guards against a
    // garbage value cast into the enum (deserialization, tooling).
    constexpr bool Is_known_type(Primitive_Type _type)
    {
        return static_cast<uint16_t>(_type) <= static_cast<uint16_t>(PRIMITIVE_TYPE_LAST);
    }

    // =========================================================
    // Parameter specification — SINGLE SOURCE OF TRUTH
    // =========================================================
    //
    // Every rule about "which parameters does this primitive actually
    // read, and what values are legal" lives here and nowhere else.
    // Primitive_Desc (key packing, validation) and Primitive_Builder
    // (generator clamps) both derive from this table, so the key can
    // never describe geometry different from what gets built.

    // Number of topological parameters a Primitive_Desc carries.
    inline constexpr uint8_t MAX_PARAMS = 3;

    // Upper bound shared by every topological parameter. A cap is needed
    // because vertex counts grow quadratically (a sphere is roughly
    // segments × rings vertices): 512 is already ~263k vertices, far past
    // anything useful, and an uncapped uint16_t would be ~4.3 billion.
    inline constexpr uint16_t MAX_TOPOLOGY_PARAM = 512;

    // Param_Range: the inclusive [min, max] a parameter may take.
    // min is also the floor the generator clamps to, so requesting less
    // than min and requesting exactly min build the identical mesh.
    struct Param_Range
    {
        uint16_t min = 0;
        uint16_t max = 0;
    };

    // Param_Spec: how many of param1..param3 a type reads, plus the legal
    // range of each. Parameters at or past used_count do not affect the
    // generated geometry at all (a Cube reads none of them).
    struct Param_Spec
    {
        uint8_t     used_count = 0;
        Param_Range range[MAX_PARAMS] = {};
    };

    // Returns the parameter contract of a primitive type.
    // An unknown type reports "no parameters used", so a garbage desc
    // still canonicalizes to something harmless instead of packing junk
    // into the cache key.
    constexpr Param_Spec Spec_of(Primitive_Type _type)
    {
        constexpr uint16_t MAX = MAX_TOPOLOGY_PARAM;

        switch (_type)
        {
            // Hardcoded geometry — the generators take no arguments, so
            // NO parameter may be set on these types.
        case Primitive_Type::Cube:
        case Primitive_Type::Quad:
        case Primitive_Type::Triangle:
        case Primitive_Type::Tetrahedron:
            return Param_Spec{ 0, {} };

            // Minimums mirror the clamps inside each generator:
            // below them the mesh is degenerate (a 2-segment cone is flat).
        case Primitive_Type::Plane:    return Param_Spec{ 1, { { 1, MAX } } };
        case Primitive_Type::Sphere:   return Param_Spec{ 2, { { 3, MAX }, { 2, MAX } } };
        case Primitive_Type::Cone:     return Param_Spec{ 1, { { 3, MAX } } };
        case Primitive_Type::Cylinder: return Param_Spec{ 1, { { 3, MAX } } };
        case Primitive_Type::Torus:    return Param_Spec{ 2, { { 3, MAX }, { 3, MAX } } };
        case Primitive_Type::Capsule:  return Param_Spec{ 2, { { 3, MAX }, { 2, MAX } } };
        }

        return Param_Spec{};
    }

    // Human-readable type name — for logs and cache source strings.
    constexpr const char* Type_name(Primitive_Type _type)
    {
        switch (_type)
        {
        case Primitive_Type::Cube:        return "cube";
        case Primitive_Type::Quad:        return "quad";
        case Primitive_Type::Triangle:    return "triangle";
        case Primitive_Type::Tetrahedron: return "tetrahedron";
        case Primitive_Type::Plane:       return "plane";
        case Primitive_Type::Sphere:      return "sphere";
        case Primitive_Type::Cone:        return "cone";
        case Primitive_Type::Cylinder:    return "cylinder";
        case Primitive_Type::Torus:       return "torus";
        case Primitive_Type::Capsule:     return "capsule";
        }

        return "unknown";
    }

    // Desc_Error: why a descriptor is not canonical. Returned by
    // Primitive_Desc::Validate() for paths that must not assert —
    // scene files, editor input, network data.
    enum class Desc_Error : uint8_t
    {
        None = 0,
        Unknown_type,       // type is not a valid Primitive_Type
        Unused_param_set,   // a parameter the type ignores is non-zero
        Param_below_min,    // a used parameter is below its legal minimum
        Param_above_max,    // a used parameter is above its legal maximum
    };

    constexpr const char* Error_message(Desc_Error _error)
    {
        switch (_error)
        {
        case Desc_Error::None:             return "ok";
        case Desc_Error::Unknown_type:     return "unknown primitive type";
        case Desc_Error::Unused_param_set: return "a parameter is set on a type that ignores it "
            "(e.g. Cube with param1 != 0)";
        case Desc_Error::Param_below_min:  return "a parameter is below the minimum the generator clamps to";
        case Desc_Error::Param_above_max:  return "a parameter is above MAX_TOPOLOGY_PARAM";
        }

        return "unknown error";
    }

    // Primitive_Desc: fully describes a primitive to generate.
    //
    // Only TOPOLOGICAL parameters belong here — values that change the
    // number or arrangement of vertices (segments, rings, subdivisions).
    // DIMENSIONAL values (radius, height, width) are NOT here: primitives
    // are unit-sized and scaled via Transform.
    //
    // ── Descriptor aliasing (why Canonical() exists) ──────────────
    //
    // Two DIFFERENT descriptors can describe the SAME geometry:
    //
    //   {Cube, 1, 999} and {Cube, 23, 214}   — Cube reads no parameters
    //   {Sphere, 1, 1} and {Sphere, 3, 2}    — the generator clamps to 3/2
    //
    // Keying the cache on the raw fields would store the same mesh twice
    // under two keys: wasted VRAM, a broken "same primitive == same
    // handle" guarantee, and duplicated draw setup.
    //
    // The fix is to key on the CANONICAL descriptor instead of the raw
    // one: unused parameters collapse to 0 and used ones are clamped into
    // their legal range, so "same geometry" and "same key" become the same
    // statement, by construction. To_key() does this for you.
    //
    // Canonicalization keeps the CACHE correct; the asserts keep the
    // CALLER honest. Both are needed — asserts vanish under NDEBUG, so
    // they can never be what guarantees the cache is duplicate-free.
    //
    // ── How to build one ──────────────────────────────────────────
    //
    //   From code (preferred): use the Make_* named constructors. They
    //   take exactly the parameters the type reads, so passing segments
    //   to a Cube is not something you can express:
    //
    //       Primitive_Desc::Make_cube()          // no params to get wrong
    //       Primitive_Desc::Make_sphere(16, 8)
    //
    //   From data (scene files, editor, network): validate instead of
    //   asserting — external data is not a programmer error:
    //
    //       if (desc.Validate() != Desc_Error::None)
    //           Log_warning(Error_message(desc.Validate()));
    //       desc = desc.Canonical();
    struct Primitive_Desc
    {
        Primitive_Type type = Primitive_Type::Cube;
        uint16_t       param1 = 0;   // segments / subdivisions
        uint16_t       param2 = 0;   // rings
        uint16_t       param3 = 0;   // reserved for future primitives

        friend constexpr bool operator==(const Primitive_Desc&, const Primitive_Desc&) = default;

        // =========================================================
        // Named constructors
        // =========================================================
        //
        // The first line of defence: a type that reads no parameters has
        // no parameters in its signature, so {Cube, 1, 999} is not a
        // mistake that can be made. Each one returns a canonical desc.

        static constexpr Primitive_Desc Make_cube() { return Make(Primitive_Type::Cube, 0, 0); }
        static constexpr Primitive_Desc Make_quad() { return Make(Primitive_Type::Quad, 0, 0); }
        static constexpr Primitive_Desc Make_triangle() { return Make(Primitive_Type::Triangle, 0, 0); }
        static constexpr Primitive_Desc Make_tetrahedron() { return Make(Primitive_Type::Tetrahedron, 0, 0); }

        static constexpr Primitive_Desc Make_plane(uint16_t _subdivisions)
        {
            return Make(Primitive_Type::Plane, _subdivisions, 0);
        }

        static constexpr Primitive_Desc Make_sphere(uint16_t _segments, uint16_t _rings)
        {
            return Make(Primitive_Type::Sphere, _segments, _rings);
        }

        static constexpr Primitive_Desc Make_cone(uint16_t _segments)
        {
            return Make(Primitive_Type::Cone, _segments, 0);
        }

        static constexpr Primitive_Desc Make_cylinder(uint16_t _segments)
        {
            return Make(Primitive_Type::Cylinder, _segments, 0);
        }

        static constexpr Primitive_Desc Make_torus(uint16_t _segments, uint16_t _rings)
        {
            return Make(Primitive_Type::Torus, _segments, _rings);
        }

        static constexpr Primitive_Desc Make_capsule(uint16_t _segments, uint16_t _rings)
        {
            return Make(Primitive_Type::Capsule, _segments, _rings);
        }

        // =========================================================
        // Validation and canonicalization
        // =========================================================

        // Returns why this descriptor is not canonical, or None.
        // Never asserts — use it on data you do not control.
        constexpr Desc_Error Validate() const
        {
            if (!Is_known_type(type))
                return Desc_Error::Unknown_type;

            const Param_Spec  spec = Spec_of(type);
            const uint16_t    params[MAX_PARAMS] = { param1, param2, param3 };

            for (uint8_t i = 0; i < MAX_PARAMS; ++i)
            {
                // Past used_count the generator never reads the value, so
                // the only value that may be stored there is 0.
                if (i >= spec.used_count)
                {
                    if (params[i] != 0)
                        return Desc_Error::Unused_param_set;

                    continue;
                }

                if (params[i] < spec.range[i].min)
                    return Desc_Error::Param_below_min;

                if (params[i] > spec.range[i].max)
                    return Desc_Error::Param_above_max;
            }

            return Desc_Error::None;
        }

        // True when this descriptor is already the canonical form of its
        // geometry, i.e. Canonical() would change nothing.
        constexpr bool Is_canonical() const
        {
            return Validate() == Desc_Error::None;
        }

        // Returns the one descriptor that stands for this geometry:
        // parameters the type ignores are zeroed, parameters it reads are
        // clamped to the same range the generator clamps to.
        //
        // The property that makes deduplication work:
        //   Build(a) == Build(b)  <=>  a.Canonical() == b.Canonical()
        constexpr Primitive_Desc Canonical() const
        {
            // An unknown type keeps its value rather than being silently
            // rewritten into a Cube: Spec_of() reports no parameters for
            // it, so the params are zeroed and the type still reaches
            // Primitive_Builder::Build(), which rejects it out loud.
            const Param_Spec spec = Spec_of(type);

            Primitive_Desc out;
            out.type = type;
            out.param1 = Canonical_param(spec, 0, param1);
            out.param2 = Canonical_param(spec, 1, param2);
            out.param3 = Canonical_param(spec, 2, param3);

            return out;
        }

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
        //
        // The key is packed from the CANONICAL descriptor, never from the
        // raw fields, so descriptors that build the same mesh produce the
        // same key and the cache cannot hold duplicates. Combined with
        // distinct canonical forms mapping to distinct integers, the key
        // is exactly one-to-one with the geometry — no hashing involved.
        //
        // In a debug build a non-canonical descriptor asserts instead of
        // being silently repaired, so the caller learns that the value
        // they passed is being ignored.
        constexpr uint64_t To_key() const
        {
            assert(Is_known_type(type) &&
                "Primitive_Desc::To_key: unknown primitive type");

            assert(Is_canonical() &&
                "Primitive_Desc::To_key: non-canonical descriptor — a parameter is out of "
                "range, or set on a type that ignores it (e.g. Cube with param1 != 0). Two "
                "such descriptors would build the same mesh under different keys. Build it "
                "with Primitive_Desc::Make_cube() / Make_sphere(s, r) / ..., or call "
                "Canonical() first; Validate() reports which rule was broken.");

            // param3 only has 15 bits (bit 63 is the discriminator), so a
            // value >= 0x8000 would silently lose its top bit and collide
            // with a different descriptor. The cap keeps that impossible.
            static_assert(MAX_TOPOLOGY_PARAM <= 0x7FFF,
                "MAX_TOPOLOGY_PARAM must fit in the 15-bit param3 field");

            constexpr uint64_t PRIMITIVE_DISCRIMINATOR = (uint64_t(1) << 63);

            const Primitive_Desc c = Canonical();

            return PRIMITIVE_DISCRIMINATOR
                | static_cast<uint64_t>(c.type)
                | (static_cast<uint64_t>(c.param1) << 16)
                | (static_cast<uint64_t>(c.param2) << 32)
                | (static_cast<uint64_t>(c.param3) << 48 & 0x7FFF000000000000ull);
        }

    private:

        // Canonical value of one parameter: 0 when the type ignores it,
        // clamped into the legal range when it does not.
        static constexpr uint16_t Canonical_param(const Param_Spec& _spec, uint8_t _index, uint16_t _value)
        {
            if (_index >= _spec.used_count)
                return 0;

            if (_value < _spec.range[_index].min)
                return _spec.range[_index].min;

            if (_value > _spec.range[_index].max)
                return _spec.range[_index].max;

            return _value;
        }

        // Shared body of the Make_* named constructors: asserts in debug
        // that the caller asked for something legal, and returns the
        // canonical descriptor either way.
        static constexpr Primitive_Desc Make(Primitive_Type _type, uint16_t _param1, uint16_t _param2)
        {
            const Primitive_Desc raw{ _type, _param1, _param2, 0 };

            assert(raw.Is_canonical() &&
                "Primitive_Desc::Make_*: parameter out of range — below the minimum the "
                "generator clamps to, or above MAX_TOPOLOGY_PARAM. The value has been "
                "clamped; Validate() reports which rule was broken.");

            return raw.Canonical();
        }
    };

    // =========================================================
    // Compile-time proof of the deduplication guarantee
    // =========================================================
    //
    // These are the aliasing cases that used to produce duplicate cache
    // entries. They run at compile time, so a future edit to Spec_of()
    // or To_key() that reintroduces the bug breaks the build.
    //
    // Note they key the CANONICAL form, the way Create_primitive() does:
    // calling To_key() on the raw descriptor is what the assert forbids,
    // and in a constant expression an assert that fires is a compile
    // error — so a literal bad descriptor is caught before it ever runs.

    // A type that reads no parameters ignores every parameter, so all
    // descriptors of that type collapse to one key.
    static_assert(Primitive_Desc{ Primitive_Type::Cube, 1, 999, 0 }.Canonical().To_key() ==
        Primitive_Desc{ Primitive_Type::Cube, 23, 214, 0 }.Canonical().To_key(),
        "Cube ignores its parameters: both descriptors must produce one key");

    static_assert(Primitive_Desc{ Primitive_Type::Cube, 1, 999, 0 }.Canonical() ==
        Primitive_Desc::Make_cube(),
        "The canonical form of a parametrized Cube is the parameterless Cube");

    // ...and it is still rejected in a debug build, rather than repaired
    // behind the caller's back.
    static_assert(!Primitive_Desc{ Primitive_Type::Cube, 1, 999, 0 }.Is_canonical(),
        "A Cube with parameters must be reported as non-canonical");

    static_assert(Primitive_Desc{ Primitive_Type::Cube, 1, 999, 0 }.Validate() ==
        Desc_Error::Unused_param_set,
        "A Cube with parameters must report Unused_param_set");

    // Values below the generator's clamp build the clamped mesh, so they
    // must key as the clamped mesh (Build_sphere clamps to 3 segments /
    // 2 rings) — the same bug as the Cube case, in a different disguise.
    static_assert(Primitive_Desc{ Primitive_Type::Sphere, 1, 1, 0 }.Canonical().To_key() ==
        Primitive_Desc::Make_sphere(3, 2).To_key(),
        "A sub-minimum sphere must key as the mesh the generator actually builds");

    // Parameters a parametrized type does not read are ignored too:
    // Cone reads param1 only.
    static_assert(Primitive_Desc{ Primitive_Type::Cone, 12, 77, 0 }.Canonical().To_key() ==
        Primitive_Desc::Make_cone(12).To_key(),
        "Cone ignores param2: it must not reach the key");

    // Distinct geometry must still key distinctly — canonicalization must
    // not over-merge.
    static_assert(Primitive_Desc::Make_sphere(16, 8).To_key() !=
        Primitive_Desc::Make_sphere(16, 9).To_key(),
        "Different rings must produce different keys");

    static_assert(Primitive_Desc::Make_sphere(16, 8).To_key() !=
        Primitive_Desc::Make_torus(16, 8).To_key(),
        "Different types must produce different keys");

    // param3 is packed into 15 bits: a value >= 0x8000 would lose its top
    // bit and key as a DIFFERENT descriptor (the mirror image of this bug —
    // one key, two geometries). Canonicalization caps it out of reach.
    static_assert(Primitive_Desc{ Primitive_Type::Cube, 0, 0, 0x8000 }.Canonical().param3 == 0,
        "param3 must never reach the key with bit 15 set");

    // Primitive keys must never land in the file-asset key space.
    static_assert((Primitive_Desc::Make_cube().To_key() >> 63) == 1,
        "Primitive keys must have the discriminator bit set");

} // namespace ResourceManager