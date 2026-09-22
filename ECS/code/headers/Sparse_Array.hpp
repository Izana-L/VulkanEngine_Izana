#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace ECS
{
    // Sparse_Array: a dynamically-allocated array indexed by arbitrary
    // integer indices (entity slots), organized in fixed-size segments
    // created on demand. Only segments that are actually accessed are
    // allocated, so sparse usage patterns don't waste memory.
    //
    // Uses a 64-bit bitmap per segment instead of std::optional per
    // element, saving sizeof(T) bytes per element (no per-element
    // has_value flag) and enabling fast iteration via bit scanning.
    //
    // Every accessor checks the bitmap in every build configuration:
    // reading an empty slot returns uninitialized bytes, which as an index
    // into a dense array is memory corruption, so it is refused with an
    // exception rather than guarded by a debug-only assertion.
    template< typename VALUE_TYPE >
    class Sparse_Array
    {
    public:

        using Value_Type = VALUE_TYPE;

    private:

        static constexpr size_t   segment_size = 64;
        static constexpr size_t   segment_shift = 6;   // log2(segment_size)
        static constexpr size_t   segment_mask = 63;   // segment_size - 1
        static constexpr uint64_t empty_bitmap = 0ull;
        static constexpr uint64_t full_bitmap = ~uint64_t{ 0 };

        static_assert(segment_size == 64, "The bitmap is a uint64_t: one bit per element");

        struct Segment
        {
            // Raw storage for segment_size elements of VALUE_TYPE,
            // without constructing them - elements are only constructed
            // when actually assigned via Set().
            alignas(VALUE_TYPE) std::byte data[segment_size * sizeof(VALUE_TYPE)];

            // One bit per element: bit i is 1 if element i has a value,
            // 0 if it's empty. Replaces the per-element bool in optional<T>.
            uint64_t bitmap = empty_bitmap;

            ~Segment()
            {
                // Destroy only elements that were actually constructed
                for (size_t i = 0; i < segment_size; ++i)
                {
                    if ((bitmap >> i) & 1u)
                    {
                        Get_element(i).~VALUE_TYPE();
                    }
                }
            }

            VALUE_TYPE& Get_element(size_t local_index)
            {
                return *std::launder(reinterpret_cast<VALUE_TYPE*>(data) + local_index);
            }

            const VALUE_TYPE& Get_element(size_t local_index) const
            {
                return *std::launder(reinterpret_cast<const VALUE_TYPE*>(data) + local_index);
            }

            bool Has(size_t local_index) const
            {
                return (bitmap >> local_index) & 1u;
            }

            bool Is_full() const
            {
                return bitmap == full_bitmap;
            }

            void Set(size_t local_index, const VALUE_TYPE& value)
            {
                if (Has(local_index))
                {
                    // Already exists: just overwrite
                    Get_element(local_index) = value;
                }
                else
                {
                    // Construct in place
                    new (reinterpret_cast<VALUE_TYPE*>(data) + local_index) VALUE_TYPE(value);
                    bitmap |= (1ull << local_index);
                }
            }

            void Unset(size_t local_index)
            {
                if (Has(local_index))
                {
                    Get_element(local_index).~VALUE_TYPE();
                    bitmap &= ~(1ull << local_index);
                }
            }
        };

        using Segment_Pointer = std::unique_ptr< Segment >;

        class Collection
        {
            std::vector< Segment_Pointer > segments;

        public:

            bool Has(size_t element_index) const
            {
                size_t segment_index = element_index >> segment_shift;
                if (segment_index >= segments.size()) return false;
                const auto& segment = segments[segment_index];
                if (!segment) return false;
                return segment->Has(element_index & segment_mask);
            }

            // Precondition: Has(element_index). Checked by the callers.
            VALUE_TYPE& Get(size_t element_index)
            {
                return segments[element_index >> segment_shift]
                    ->Get_element(element_index & segment_mask);
            }

            const VALUE_TYPE& Get(size_t element_index) const
            {
                return segments[element_index >> segment_shift]
                    ->Get_element(element_index & segment_mask);
            }

            void Set(size_t element_index, const VALUE_TYPE& value)
            {
                size_t segment_index = element_index >> segment_shift;

                if (segment_index >= segments.size())
                {
                    segments.resize(segment_index + 1);
                }

                auto& segment = segments[segment_index];
                if (!segment) segment = std::make_unique< Segment >();

                segment->Set(element_index & segment_mask, value);
            }

            void Unset(size_t element_index)
            {
                size_t segment_index = element_index >> segment_shift;
                if (segment_index >= segments.size()) return;
                auto& segment = segments[segment_index];
                if (!segment) return;
                segment->Unset(element_index & segment_mask);
            }
        };

    private:

        Collection collection;

        [[noreturn]] static void Throw_empty_slot(size_t index)
        {
            throw std::out_of_range(
                "Sparse_Array: slot " + std::to_string(index) + " holds no value");
        }

    public:

        // Returns true if the given index has a value assigned
        bool Has_value(size_t index) const
        {
            return collection.Has(index);
        }

        // Returns a reference to the value at index.
        // Throws std::out_of_range if the slot is empty.
        VALUE_TYPE& operator [] (size_t index)
        {
            if (!collection.Has(index)) Throw_empty_slot(index);
            return collection.Get(index);
        }

        const VALUE_TYPE& operator [] (size_t index) const
        {
            if (!collection.Has(index)) Throw_empty_slot(index);
            return collection.Get(index);
        }

        // Returns a pointer to the value at index, or nullptr if empty.
        // The non-throwing lookup for hot paths that test presence anyway.
        VALUE_TYPE* Find(size_t index)
        {
            return collection.Has(index) ? &collection.Get(index) : nullptr;
        }

        const VALUE_TYPE* Find(size_t index) const
        {
            return collection.Has(index) ? &collection.Get(index) : nullptr;
        }

        // Assigns a value to the given index (creates or overwrites)
        void Set(size_t index, const VALUE_TYPE& value)
        {
            collection.Set(index, value);
        }

        // Removes the value at the given index (no-op if empty)
        void Unset(size_t index)
        {
            collection.Unset(index);
        }

    };

}
