#pragma once

#include <vector>
#include <Id.hpp>

namespace CoreTypes
{

    // Id_Provider: manages a pool of reusable numeric IDs.
    // Allocates IDs sequentially on first use, and reuses released IDs
    // via a free-list (linked list of available nodes) so that Entity
    // slots can be recycled without gaps or linear searches.
    //
    // Memory is organized in fixed-size segments of 256 IDs each,
    // avoiding reallocations of existing segments as the pool grows.
    class Id_Provider
    {
        static constexpr size_t pool_capacity = 32;     // segments reserved upfront
        static constexpr size_t segment_size = 256;    // IDs per segment
        static constexpr size_t segment_shift = 8;      // log2(segment_size)
        static constexpr size_t segment_mask = 255;    // segment_size - 1

        struct Node
        {
            Node* next;
            Id     id;
        };

        class Segment
        {
            std::vector< Node > nodes;

        public:

            Segment(size_t segment_index) : nodes{ segment_size }
            {
                Id id = static_cast<Id>(segment_index * segment_size);

                for (auto& node : nodes) node = { &node + 1, id++ };

                nodes.back().next = nullptr;
            }

            Node& first_node()
            {
                return nodes.front();
            }

            Node& operator [] (Id index)
            {
                return nodes[index];
            }
        };

        class Pool
        {
            std::vector< Segment > segments;

        public:

            Pool()
            {
                segments.reserve(pool_capacity);
            }

            // Adds a new segment and returns a pointer to its first node.
            // The pointer stays valid even if segments grows, because each
            // Segment owns its own std::vector - only the vector of
            // segments itself can reallocate, not its elements' contents.
            Node* Extend()
            {
                segments.emplace_back(segments.size());
                return &segments.back().first_node();
            }

            Node& operator [] (Id index)
            {
                return segments[index >> segment_shift][index & segment_mask];
            }
        };

    private:

        Pool   pool;
        Node* first_free_node;

    public:

        Id_Provider() : first_free_node(nullptr)
        {}

        // Returns a unique ID, reusing a previously released one if
        // available, or extending the pool to get a fresh one.
        Id Allocate_id()
        {
            if (!first_free_node) first_free_node = pool.Extend();

            Id id = first_free_node->id;
            first_free_node = first_free_node->next;

            return id;
        }

        // Returns an ID back to the free list for future reuse.
        // After release, the ID must not be used again until re-allocated.
        void Release(const Id id)
        {
            Node& node = pool[id];
            node.next = first_free_node;
            first_free_node = &node;
        }
    };

}