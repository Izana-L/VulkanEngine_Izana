#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace CoreTypes
{

    // Hash_combine: folds one hash value into a running seed, in the
    // boost::hash_combine style.
    //
    // This is the single implementation shared by every hash functor in
    // the engine (Sampler_Desc_Hash, Pipeline_Config_Hash, ...). Having one
    // copy matters for a reason beyond tidiness: the mixing constant must
    // match the width of size_t. The 32-bit golden-ratio constant
    // 0x9e3779b9 applied to a 64-bit seed only stirs the low half of the
    // word, which weakens the distribution of every map keyed by it.
    namespace internal
    {
        template< size_t WIDTH >
        struct Golden_Ratio;

        template<>
        struct Golden_Ratio< 4 >
        {
            static constexpr size_t value = 0x9e3779b9u;
        };

        template<>
        struct Golden_Ratio< 8 >
        {
            static constexpr size_t value = static_cast<size_t>(0x9e3779b97f4a7c15ull);
        };
    }

    inline void Hash_combine(size_t& _seed, size_t _value)
    {
        constexpr size_t golden = internal::Golden_Ratio< sizeof(size_t) >::value;

        _seed ^= _value + golden + (_seed << 6) + (_seed >> 2);
    }

    // Convenience overload: hashes _value with std::hash before folding it.
    template< typename TYPE >
    inline void Hash_combine_value(size_t& _seed, const TYPE& _value)
    {
        Hash_combine(_seed, std::hash< TYPE >{}(_value));
    }

} // namespace CoreTypes
