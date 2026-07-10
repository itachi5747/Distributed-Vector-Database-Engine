#pragma once

// ──────────────────────────────────────────────────────────────────────────────
//  ConsistentHashRing — Phase 5
//
//  Maps vector_id → shard_index using a sorted ring of virtual nodes.
//
//  Why consistent hashing?
//    With naive modulo hashing (id % N), adding or removing a shard requires
//    rehoming almost every vector. Consistent hashing limits rehoming to
//    approximately (1/N) of the total data when a shard is added/removed.
//
//  How it works:
//    Each shard is assigned `virtual_nodes` positions on a 64-bit hash ring.
//    A vector_id is hashed to a point on the ring; it belongs to the first
//    shard whose virtual node falls at or after that point (clockwise).
//
//  Virtual nodes:
//    More virtual nodes → more uniform load distribution.
//    Default 150 per shard gives <5% load imbalance in practice.
//
//  Thread safety:
//    The ring is immutable after construction. No locks needed for lookup.
//    Rebalancing (add/remove shard) creates a new ring and swaps atomically.
// ──────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <vector>
#include <string>

namespace vecdb {

class ConsistentHashRing {
public:
    // Construct a ring for `shard_count` shards with `virtual_nodes` per shard.
    ConsistentHashRing(uint32_t shard_count, uint32_t virtual_nodes = 150);

    // Map a vector_id to its owning shard index [0, shard_count).
    uint32_t get_shard(uint64_t vector_id) const;

    // Map a raw string key to its owning shard index.
    uint32_t get_shard(const std::string& key) const;

    // Number of shards on this ring.
    uint32_t shard_count() const { return shard_count_; }

    // Number of virtual nodes per shard.
    uint32_t virtual_nodes() const { return virtual_nodes_; }

    // Return all shard indices that would be affected if a new shard were added.
    // Used during rebalancing to identify vectors that need to move.
    std::vector<uint32_t> affected_shards_on_add() const;

private:
    struct VNode {
        uint64_t hash;      // position on the ring
        uint32_t shard_id;  // owning shard
    };

    uint32_t            shard_count_;
    uint32_t            virtual_nodes_;
    std::vector<VNode>  ring_;   // sorted by hash

    static uint64_t hash_key(const void* data, size_t len);
    static uint64_t hash_vnode(uint32_t shard_id, uint32_t vnode_idx);
};

} // namespace vecdb
