#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace CoreTypes
{
    namespace internal
    {
        constexpr uint32_t fnv_basis_32 = 0x811c9dc5u;
        constexpr uint32_t fnv_prime_32 = 0x01000193u;
        constexpr uint64_t fnv_basis_64 = 0xcbf29ce484222325u;
        constexpr uint64_t fnv_prime_64 = 0x00000100000001b3u;

        // FNV-1a over a byte range. This is the ONLY implementation in the
        // file: every entry point below is a one-line wrapper around it.
        //
        // The previous version had three separate loops that disagreed with
        // each other — one hashed the trailing '\0', another sign-extended
        // bytes >= 0x80 — so the same text produced different keys depending
        // on which overload you happened to call.
        //
        // The cast to uint8_t is not cosmetic: char is signed on MSVC, and
        // without it any non-ASCII byte flips the high bits of the hash.
        constexpr uint32_t fnv32_bytes(const char* chars, size_t count)
        {
            uint32_t hash = fnv_basis_32;

            for (size_t index = 0; index < count; ++index)
            {
                hash ^= static_cast<uint8_t>(chars[index]);
                hash *= fnv_prime_32;
            }

            return hash;
        }

        constexpr uint64_t fnv64_bytes(const char* chars, size_t count)
        {
            uint64_t hash = fnv_basis_64;

            for (size_t index = 0; index < count; ++index)
            {
                hash ^= static_cast<uint8_t>(chars[index]);
                hash *= fnv_prime_64;
            }

            return hash;
        }
    }

    // Runtime entry points. string_view, so a std::string, a literal and a
    // char* all land here with no overload to pick and no ambiguity left.
    constexpr uint32_t fnv32(std::string_view text)
    {
        return internal::fnv32_bytes(text.data(), text.size());
    }

    constexpr uint64_t fnv64(std::string_view text)
    {
        return internal::fnv64_bytes(text.data(), text.size());
    }

    // Compile-time entry points, for string literals. LENGTH - 1 drops the
    // terminator, which is what makes them agree with the ones above.
    template< size_t LENGTH >
    constexpr uint32_t static_fnv32(const char(&chars)[LENGTH])
    {
        return internal::fnv32_bytes(chars, LENGTH - 1);
    }

    template< size_t LENGTH >
    constexpr uint64_t static_fnv64(const char(&chars)[LENGTH])
    {
        return internal::fnv64_bytes(chars, LENGTH - 1);
    }

    template< size_t LENGTH >
    constexpr unsigned static_fnv(const char(&chars)[LENGTH])
    {
        if constexpr (sizeof(unsigned) == 4)
        {
            return static_fnv32(chars);
        }
        else
        {
            return static_cast<unsigned>(static_fnv64(chars));
        }
    }

    // The property the asset cache depends on, checked by the compiler.
    // These are what stop the three-way divergence from coming back: break
    // any loop bound or drop the uint8_t cast and the build fails here.
    static_assert(static_fnv32("shader") == fnv32("shader"));
    static_assert(static_fnv64("shader") == fnv64("shader"));
    static_assert(static_fnv32("\xC3\xB1") == fnv32("\xC3\xB1"));
    static_assert(fnv32("") == internal::fnv_basis_32);
}

#define FNV(X)   CoreTypes::static_fnv   (#X)
#define FNV32(X) CoreTypes::static_fnv32 (#X)
#define FNV64(X) CoreTypes::static_fnv64 (#X)