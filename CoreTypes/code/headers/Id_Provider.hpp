#pragma once

#include <Id.hpp>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace CoreTypes
{

    // Id_Provider: manages a pool of reusable numeric IDs.
    //
    // IDs are handed out sequentially on first use and recycled through a
    // free list (an intrusive singly linked list of released nodes), so a
    // slot can be reused without gaps and without a linear search.
    //
    // Memory is organized in fixed-size segments of 256 IDs each. A new
    // segment is allocated only when the free list is exhausted, and an
    // existing segment is never reallocated, so a Node* stays valid for
    // the lifetime of the provider.
    //
    // Every node carries an `allocated` flag. It is what makes the free
    // list robust: releasing an ID that is not allocated, or an ID that
    // was never handed out, is detected and rejected instead of threading
    // the node into the list twice (which would make every later
    // Allocate_id() return the same value forever).
    class Id_Provider
    {
        static constexpr size_t pool_capacity = 32;    // segments reserved upfront
        static constexpr size_t segment_size = 256;    // IDs per segment
        static constexpr size_t segment_shift = 8;     // log2(segment_size)
        static constexpr size_t segment_mask = 255;    // segment_size - 1

        struct Node
        {
            Node* next = nullptr;
            Id    id = INVALID_ID;
            uint32_t generation = 0;
            bool  allocated = false;
        };

        class Segment
        {
            std::vector< Node > nodes;

        public:

            explicit Segment(size_t segment_index) : nodes(segment_size)
            {
                Id id = static_cast<Id>(segment_index * segment_size);

                for (size_t i = 0; i < segment_size; ++i)
                {
                    nodes[i].next = (i + 1 < segment_size) ? &nodes[i + 1] : nullptr;
                    nodes[i].id = id++;
                    nodes[i].allocated = false;
                }
            }

            Node& first_node()
            {
                return nodes.front();
            }

            Node& operator [] (size_t index)
            {
                return nodes[index];
            }

            const Node& operator [] (size_t index) const
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
            // The pointer stays valid even if the vector of segments
            // reallocates, because each Segment owns its own std::vector
            // of nodes: only the outer vector moves, never the nodes.
            Node* Extend()
            {
                segments.emplace_back(segments.size());
                return &segments.back().first_node();
            }

            // Number of IDs that currently have a backing node.
            size_t Capacity() const
            {
                return segments.size() * segment_size;
            }

            void Clear()
            {
                segments.clear();
            }

            //Frees every node and threads the free list through all segments in
            // index order. A node that was allocated gets its generation advanced,
            // so every ID handed out before the call is stale afterwards.
            // Returns the head of the new free list (nullptr without segments).
            Node * Invalidate_all()
            {
                Node* head = nullptr;
                Node* tail = nullptr;

                for (Segment& segment : segments)
                {
                    for (size_t i = 0; i < segment_size; ++i)
                    {
                        Node& node = segment[i];

                        if (node.allocated)
                        {
                            node.allocated = false;
                            ++node.generation;
                        }

                        node.next = nullptr;

                        if (tail) tail->next = &node;
                        else      head = &node;

                        tail = &node;
                    }
                }

                return head;
            }

            Node& operator [] (Id index)
            {
                return segments[index >> segment_shift][index & segment_mask];
            }

            const Node& operator [] (Id index) const
            {
                return segments[index >> segment_shift][index & segment_mask];
            }
        };

    private:

        Pool   pool;
        Node*  first_free_node = nullptr;
        size_t allocated_count = 0;

    public:

        Id_Provider() = default;

        // Returns a unique ID, reusing a previously released one if
        // available, or extending the pool to get a fresh one.
        Id Allocate_id()
        {
            if (!first_free_node) first_free_node = pool.Extend();

            Node& node = *first_free_node;
            first_free_node = node.next;

            node.next = nullptr;
            node.allocated = true;
            ++allocated_count;

            return node.id;
        }

        // Returns an ID to the free list for future reuse.
        //
        // Returns false, and leaves the provider untouched, when the ID
        // was never handed out by this provider or has already been
        // released. Both cases are caller bugs; rejecting them keeps the
        // free list acyclic, which is the invariant Allocate_id() relies on.
        bool Release(const Id id)
        {
            if (!Is_allocated(id)) return false;

            Node& node = pool[id];
            node.allocated = false;
            ++node.generation;
            node.next = first_free_node;
            first_free_node = &node;
            --allocated_count;

            return true;
        }

        // True if the ID was handed out by Allocate_id() and has not been
        // released since. An ID outside the pool is reported as not allocated.
        bool Is_allocated(const Id id) const
        {
            if (Not_valid(id)) return false;
            if (static_cast<size_t>(id) >= pool.Capacity()) return false;

            return pool[id].allocated;
        }
        // Generation the slot currently carries. A handle for a live ID must
        // carry this value. An ID outside the pool reports 0.
        uint32_t Generation(const Id id) const
        {
            if (Not_valid(id)) return 0;
            if (static_cast<size_t>(id) >= pool.Capacity()) return 0;

            return pool[id].generation;
        }

        // True if the ID is allocated AND the generation matches: the check
        // that detects a handle kept after its slot was released and reused.
        bool Is_current(const Id id, const uint32_t generation) const
        {
            return Is_allocated(id) && pool[id].generation == generation;
        }
        // Number of IDs currently allocated.
        size_t Allocated_count() const
        {
            return allocated_count;
        }

        // Number of IDs that have a backing node (allocated or free).
        size_t Capacity() const
        {
            return pool.Capacity();
        }
        // Forgets every allocation. The next Allocate_id() returns 0 again.
        // Callers must make sure no ID handed out before the reset is used
        // afterwards; a generation counter on the caller's side (see
        // ECS::World) is what makes that detectable.
        void Reset()
        {
            pool.Clear();
            first_free_node = nullptr;
            allocated_count = 0;
        }
        // Releases every allocation at once. Each allocated ID advances its
        // generation, so every handle handed out before the call fails
        // Is_current() afterwards. The next Allocate_id() returns 0 again.
        void Invalidate_all()
        {
            first_free_node = pool.Invalidate_all();
            allocated_count = 0;
        }
    };

}
