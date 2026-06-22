#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
namespace ECS
{
    namespace internal
    {
        constexpr uint32_t fnv_basis_32 = 0x811c9dc5u;
        constexpr uint32_t fnv_prime_32 = 0x01000193u;
        constexpr uint64_t fnv_basis_64 = 0xcbf29ce484222325u;
        constexpr uint64_t fnv_prime_64 = 0x00000100000001b3u;

        template< size_t LENGTH >
        consteval uint32_t static_fnv32(const char* chars)
        {
            return (static_fnv32< LENGTH - 1 >(chars) ^ chars[LENGTH - 2]) * fnv_prime_32;
        }

        template< >
        consteval uint32_t static_fnv32< 1 >(const char*)
        {
            return fnv_basis_32;
        }

        template< size_t LENGTH >
        consteval uint64_t static_fnv64(const char* chars)
        {
            return (static_fnv64< LENGTH - 1 >(chars) ^ chars[LENGTH - 2]) * fnv_prime_64;
        }

        template< >
        consteval uint64_t static_fnv64< 1 >(const char*)
        {
            return fnv_basis_64;
        }
    }

    template< size_t LENGTH >
    constexpr uint32_t static_fnv32(const char(&chars)[LENGTH])
    {
        return internal::static_fnv32< LENGTH >(chars);
    }

    template< size_t LENGTH >
    constexpr uint64_t static_fnv64(const char(&chars)[LENGTH])
    {
        return internal::static_fnv64< LENGTH >(chars);
    }

    template< size_t LENGTH >
    constexpr unsigned static_fnv(const char(&chars)[LENGTH])
    {
        if constexpr (sizeof(unsigned) == 4)
        {
            return internal::static_fnv32< LENGTH >(chars);
        }
        else
        {
            return static_cast<unsigned>(internal::static_fnv64< LENGTH >(chars));
        }
    }

    template< size_t LENGTH >
    uint32_t fnv32(const char(&chars)[LENGTH])
    {
        uint32_t hash = internal::fnv_basis_32;
        const char* c = chars;

        for (size_t index = 0; index < LENGTH; ++index)
        {
            hash ^= *(c++);
            hash *= internal::fnv_prime_32;
        }

        return hash;
    }

    inline uint32_t fnv32(const std::string& s)
    {
        uint32_t hash = internal::fnv_basis_32;

        for (auto c : s)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= internal::fnv_prime_32;
        }

        return hash;
    }

}

#define FNV(X)   ECS::static_fnv   (#X)
#define FNV32(X) ECS::static_fnv32 (#X)
#define FNV64(X) ECS::static_fnv64 (#X)