#pragma once

#include <Fnv.hpp>

#define ID(X) FNV(X)

namespace CoreTypes
{
    using Id = unsigned int;

    constexpr Id INVALID_ID = ~0u;

    constexpr inline bool Is_valid(Id id)
    {
        return id != INVALID_ID;
    }

    constexpr inline bool Not_valid(Id id)
    {
        return id == INVALID_ID;
    }
}