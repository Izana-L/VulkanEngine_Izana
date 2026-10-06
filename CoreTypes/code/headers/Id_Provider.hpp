#pragma once

#include <Id.hpp>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace CoreTypes
{

    // Id_Provider: manages a pool of reusable numeric IDs.
    //
    // IDs are handed out sequentially on first use and recycled through a
    // free list (an intrusive singly linked list of released nodes), so a
    // slot can be reused without gaps and without a linear search.
    //
    // The free list is linked by ID, never by pointer: a node stores the ID
    // of the next free node, and the provider stores the ID of the first
    // one. Nothing in the object holds an address of its own storage, so
    // the storage can be copied or moved freely and every link stays valid:
    //   - a copy is a fully independent provider (it never writes into the
    //     original's nodes);
    //   - a move transfers the whole state and leaves the source empty,
    //     exactly like a default-constructed provider, instead of leaving it
    //     with a free list that now belongs to the destination.
    //
    // Memory grows in steps of 256 IDs, only when the free list is
    // exhausted. Nodes may be relocated when the storage grows; that is
    // safe precisely because they are addressed by ID.
    //
    // Every node carries an `allocated` flag. It is what makes the free
    // list robust: releasing an ID that is not allocated, or an ID that
    // was never handed out, is detected and rejected instead of threading
    // the node into the list twice (which would make every later
    // Allocate_id() return the same value forever).
    class Id_Provider
    {
        static constexpr size_t growth_step = 256;     // IDs added per extension

        // Largest capacity for which every ID is still below INVALID_ID,
        // which is reserved as the "no node" link. It also guarantees that
        // INVALID_ID is never a position inside `nodes`.
        static constexpr size_t max_capacity =
            (static_cast<size_t>(INVALID_ID) / growth_step) * growth_step;

        struct Node
        {
            Id       next = INVALID_ID;     // next free node; INVALID_ID ends the list
            uint32_t generation = 0;
            bool     allocated = false;
        };

        // The ID of a node is its position in this vector.
        std::vector< Node > nodes;
        Id                  first_free = INVALID_ID;
        size_t              allocated_count = 0;

        // True if the ID has a backing node. INVALID_ID never does.
        bool Has_node(const Id id) const
        {
            return static_cast<size_t>(id) < nodes.size();
        }

        // Appends growth_step nodes, links them in ID order and makes the
        // first of them the head of the free list. Called only when the
        // free list is empty, so no free node is lost.
        // Throws std::length_error when the ID space is exhausted; the
        // provider is left unchanged in that case and when allocation fails.
        void Extend()
        {
            const size_t first = nodes.size();

            if (first >= max_capacity)
                throw std::length_error("Id_Provider: ID space exhausted");

            nodes.resize(first + growth_step);

            // The last node keeps next == INVALID_ID and ends the list.
            for (size_t i = first; i + 1 < nodes.size(); ++i)
                nodes[i].next = static_cast<Id>(i + 1);

            first_free = static_cast<Id>(first);
        }

    public:

        Id_Provider() = default;
        ~Id_Provider() = default;

        // Deep copy: the free list is made of IDs, so the copy's links
        // refer to the copy's own nodes.
        Id_Provider(const Id_Provider&) = default;
        Id_Provider& operator=(const Id_Provider&) = default;

        // The source is left empty and reusable: no allocations, no free
        // list, Allocate_id() returns 0 again.
        Id_Provider(Id_Provider&& other) noexcept
            : nodes(std::move(other.nodes))
            , first_free(std::exchange(other.first_free, INVALID_ID))
            , allocated_count(std::exchange(other.allocated_count, size_t{ 0 }))
        {
            other.nodes.clear();
        }

        Id_Provider& operator=(Id_Provider&& other) noexcept
        {
            if (this != &other)
            {
                nodes = std::move(other.nodes);
                first_free = std::exchange(other.first_free, INVALID_ID);
                allocated_count = std::exchange(other.allocated_count, size_t{ 0 });
                other.nodes.clear();
            }

            return *this;
        }

        // Returns a unique ID, reusing a previously released one if
        // available, or extending the pool to get a fresh one.
        Id Allocate_id()
        {
            if (Not_valid(first_free)) Extend();

            const Id id = first_free;
            Node& node = nodes[id];

            first_free = node.next;

            node.next = INVALID_ID;
            node.allocated = true;
            ++allocated_count;

            return id;
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

            Node& node = nodes[id];
            node.allocated = false;
            ++node.generation;
            node.next = first_free;
            first_free = id;
            --allocated_count;

            return true;
        }

        // True if the ID was handed out by Allocate_id() and has not been
        // released since. An ID outside the pool is reported as not allocated.
        bool Is_allocated(const Id id) const
        {
            return Has_node(id) && nodes[id].allocated;
        }

        // Generation the slot currently carries. A handle for a live ID must
        // carry this value. An ID outside the pool reports 0.
        uint32_t Generation(const Id id) const
        {
            return Has_node(id) ? nodes[id].generation : 0;
        }

        // True if the ID is allocated AND the generation matches: the check
        // that detects a handle kept after its slot was released and reused.
        bool Is_current(const Id id, const uint32_t generation) const
        {
            return Is_allocated(id) && nodes[id].generation == generation;
        }

        // Number of IDs currently allocated.
        size_t Allocated_count() const
        {
            return allocated_count;
        }

        // Number of IDs that have a backing node (allocated or free).
        size_t Capacity() const
        {
            return nodes.size();
        }

        // Forgets every allocation. The next Allocate_id() returns 0 again.
        // Callers must make sure no ID handed out before the reset is used
        // afterwards; a generation counter on the caller's side (see
        // ECS::World) is what makes that detectable.
        void Reset()
        {
            nodes.clear();
            first_free = INVALID_ID;
            allocated_count = 0;
        }

        // Releases every allocation at once. Each allocated ID advances its
        // generation, so every handle handed out before the call fails
        // Is_current() afterwards. The free list is rebuilt in ID order, so
        // the next Allocate_id() returns 0 again.
        void Invalidate_all()
        {
            Id head = INVALID_ID;

            // Walking backwards links each node to the one that follows it.
            for (size_t i = nodes.size(); i-- > 0;)
            {
                Node& node = nodes[i];

                if (node.allocated)
                {
                    node.allocated = false;
                    ++node.generation;
                }

                node.next = head;
                head = static_cast<Id>(i);
            }

            first_free = head;
            allocated_count = 0;
        }
    };

}
