#include "neighbour_store.hpp"

#include <stdexcept>
#include <cassert>

namespace vecdb {

// ──────────────────────────────────────────────────────────────────
//  Constructor
// ──────────────────────────────────────────────────────────────────
NeighbourStore::NeighbourStore(size_t arena_bytes)
    : arena_buf_(arena_bytes)                          // allocate backing buffer once
    , pool_(arena_buf_.data(), arena_buf_.size())      // bump allocator over that buffer
    , empty_(&pool_)                                   // empty list (returned for missing layers)
{}

// ──────────────────────────────────────────────────────────────────
//  resize
//
//  Grow adj_ so that indices [0, new_size) are valid.
//  Each new node gets `max_layers` empty pmr::vector<uint32_t> slots.
// ──────────────────────────────────────────────────────────────────
void NeighbourStore::resize(uint32_t new_size, int max_layers) {
    const uint32_t old_size = static_cast<uint32_t>(adj_.size());
    if (new_size <= old_size) return;

    adj_.resize(new_size);

    // Initialise only the new slots
    for (uint32_t i = old_size; i < new_size; ++i) {
        adj_[i].resize(max_layers);
        for (int l = 0; l < max_layers; ++l) {
            // Construct a pmr::vector backed by our arena
            adj_[i][l] = std::pmr::vector<uint32_t>(&pool_);
        }
    }
}

// ──────────────────────────────────────────────────────────────────
//  set_neighbours
// ──────────────────────────────────────────────────────────────────
void NeighbourStore::set_neighbours(uint32_t node_id, int layer,
                                     const std::vector<uint32_t>& neighbours) {
    assert(node_id < adj_.size());
    assert(layer < static_cast<int>(adj_[node_id].size()));

    auto& list = adj_[node_id][layer];
    list.clear();
    list.insert(list.end(), neighbours.begin(), neighbours.end());
}

// ──────────────────────────────────────────────────────────────────
//  add_neighbour
// ──────────────────────────────────────────────────────────────────
void NeighbourStore::add_neighbour(uint32_t node_id, int layer, uint32_t neighbour) {
    assert(node_id < adj_.size());
    assert(layer < static_cast<int>(adj_[node_id].size()));
    adj_[node_id][layer].push_back(neighbour);
}

// ──────────────────────────────────────────────────────────────────
//  get_neighbours
// ──────────────────────────────────────────────────────────────────
const std::pmr::vector<uint32_t>&
NeighbourStore::get_neighbours(uint32_t node_id, int layer) const {
    if (node_id >= adj_.size()) return empty_;
    if (layer < 0 || layer >= static_cast<int>(adj_[node_id].size())) return empty_;
    return adj_[node_id][layer];
}

// ──────────────────────────────────────────────────────────────────
//  layer_count
// ──────────────────────────────────────────────────────────────────
int NeighbourStore::layer_count(uint32_t node_id) const {
    if (node_id >= adj_.size()) return 0;
    return static_cast<int>(adj_[node_id].size());
}

// ──────────────────────────────────────────────────────────────────
//  node_count
// ──────────────────────────────────────────────────────────────────
uint32_t NeighbourStore::node_count() const {
    return static_cast<uint32_t>(adj_.size());
}

} // namespace vecdb
