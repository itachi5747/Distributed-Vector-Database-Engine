#pragma once

#include <cstdint>
#include <vector>
#include <memory_resource>  // std::pmr

namespace vecdb {

// ──────────────────────────────────────────────────────────────────
//  NeighbourStore
//
//  Stores the HNSW adjacency lists for all nodes at all layers.
//  Uses a pmr::monotonic_buffer_resource (slab / bump allocator) so:
//    - zero heap fragmentation
//    - O(1) allocation per neighbour connection
//    - no individual deallocation cost (entire arena freed at once)
//
//  Layout:
//    adj_[node_id] → vector of per-layer neighbour lists
//    adj_[node_id][layer] → pmr::vector<uint32_t> of neighbour node ids
// ──────────────────────────────────────────────────────────────────
class NeighbourStore {
public:
    // `arena_bytes`: total bytes pre-allocated for the slab.
    // A safe default for a shard of ~1M 768-dim vectors is 512 MB.
    explicit NeighbourStore(size_t arena_bytes);

    // Not copyable (owns a large arena buffer)
    NeighbourStore(const NeighbourStore&) = delete;
    NeighbourStore& operator=(const NeighbourStore&) = delete;

    // ── Mutating ──────────────────────────────────────────────────

    // Grow the store to hold at least `new_size` nodes.
    // Must be called (with exclusive lock held) before accessing node >= current size.
    void resize(uint32_t new_size, int max_layers);

    // Replace the neighbour list for (node_id, layer) with `neighbours`.
    void set_neighbours(uint32_t node_id, int layer,
                        const std::vector<uint32_t>& neighbours);

    // Append a single neighbour to (node_id, layer).
    void add_neighbour(uint32_t node_id, int layer, uint32_t neighbour);

    // ── Read-only ─────────────────────────────────────────────────

    // Returns a const reference to the neighbour list at (node_id, layer).
    // Returns an empty list if layer does not exist for this node.
    const std::pmr::vector<uint32_t>& get_neighbours(uint32_t node_id,
                                                       int layer) const;

    // Number of layers allocated for node_id (= assigned_layer + 1)
    int layer_count(uint32_t node_id) const;

    // Number of nodes currently stored
    uint32_t node_count() const;

private:
    // Backing memory for the arena (heap-allocated once)
    std::vector<std::byte> arena_buf_;

    // The bump allocator — lives on top of arena_buf_
    std::pmr::monotonic_buffer_resource pool_;

    // adj_[node_id][layer] — outer vector uses default alloc (small, ~8B/entry);
    // inner pmr::vector uses the arena for the actual neighbour id arrays.
    //
    // Why outer = std::vector (not pmr)?
    //   The outer vector is resized rarely (once per insert) and its elements
    //   are themselves vectors — pointer-sized.  The hot allocation path is the
    //   inner neighbour id arrays, which is where arena allocation matters.
    std::vector<std::vector<std::pmr::vector<uint32_t>>> adj_;

    // Shared empty list returned for missing (node, layer) pairs
    std::pmr::vector<uint32_t> empty_;
};

} // namespace vecdb
