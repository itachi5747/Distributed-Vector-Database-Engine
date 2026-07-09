#include "consistent_hash.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace vecdb {

// ── FNV-1a 64-bit hash ────────────────────────────────────────────────────────
// Fast, well-distributed, no external dependency.
// Used for both vector_id hashing and virtual node placement.

static constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
static constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

uint64_t ConsistentHashRing::hash_key(const void* data, size_t len) {
    uint64_t h = FNV_OFFSET;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= FNV_PRIME;
    }
    return h;
}

// Hash a (shard_id, vnode_idx) pair to a stable ring position.
// Using a mix of both values ensures different shards don't collide.
uint64_t ConsistentHashRing::hash_vnode(uint32_t shard_id, uint32_t vnode_idx) {
    // Pack into 8 bytes and hash
    uint64_t key = (static_cast<uint64_t>(shard_id) << 32) | vnode_idx;
    // Apply a second FNV round for better avalanche
    return hash_key(&key, sizeof(key));
}

// ── Constructor ───────────────────────────────────────────────────────────────

ConsistentHashRing::ConsistentHashRing(uint32_t shard_count, uint32_t virtual_nodes)
    : shard_count_(shard_count)
    , virtual_nodes_(virtual_nodes)
{
    if (shard_count == 0)
        throw std::invalid_argument("ConsistentHashRing: shard_count must be > 0");
    if (virtual_nodes == 0)
        throw std::invalid_argument("ConsistentHashRing: virtual_nodes must be > 0");

    // Populate ring: shard_count * virtual_nodes entries
    ring_.reserve(static_cast<size_t>(shard_count) * virtual_nodes);

    for (uint32_t s = 0; s < shard_count; ++s) {
        for (uint32_t v = 0; v < virtual_nodes; ++v) {
            ring_.push_back({ hash_vnode(s, v), s });
        }
    }

    // Sort by hash position — this IS the ring
    std::sort(ring_.begin(), ring_.end(),
              [](const VNode& a, const VNode& b) { return a.hash < b.hash; });
}

// ── get_shard (uint64_t) ──────────────────────────────────────────────────────

uint32_t ConsistentHashRing::get_shard(uint64_t vector_id) const {
    uint64_t h = hash_key(&vector_id, sizeof(vector_id));

    // Binary search for the first ring position >= h
    auto it = std::lower_bound(ring_.begin(), ring_.end(), h,
        [](const VNode& vn, uint64_t val) { return vn.hash < val; });

    // Wrap around: if past the end, use the first node (ring is circular)
    if (it == ring_.end()) it = ring_.begin();

    return it->shard_id;
}

// ── get_shard (string key) ────────────────────────────────────────────────────

uint32_t ConsistentHashRing::get_shard(const std::string& key) const {
    uint64_t h = hash_key(key.data(), key.size());

    auto it = std::lower_bound(ring_.begin(), ring_.end(), h,
        [](const VNode& vn, uint64_t val) { return vn.hash < val; });

    if (it == ring_.end()) it = ring_.begin();
    return it->shard_id;
}

// ── affected_shards_on_add ────────────────────────────────────────────────────
// Returns the unique set of shard IDs that currently own ranges that would be
// split if a new shard (shard_count_) were added to the ring.

std::vector<uint32_t> ConsistentHashRing::affected_shards_on_add() const {
    // Simulate adding one more shard: generate its virtual node positions
    const uint32_t new_shard = shard_count_;
    std::vector<uint32_t> affected;

    for (uint32_t v = 0; v < virtual_nodes_; ++v) {
        uint64_t pos = hash_vnode(new_shard, v);

        // Find which existing shard owns this position
        auto it = std::lower_bound(ring_.begin(), ring_.end(), pos,
            [](const VNode& vn, uint64_t val) { return vn.hash < val; });
        if (it == ring_.end()) it = ring_.begin();

        uint32_t owner = it->shard_id;

        // Record if not already in list
        bool found = false;
        for (uint32_t id : affected) if (id == owner) { found = true; break; }
        if (!found) affected.push_back(owner);
    }
    return affected;
}

} // namespace vecdb