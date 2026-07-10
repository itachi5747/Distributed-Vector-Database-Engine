#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <shared_mutex>
#include <random>
#include <cstdint>
#include <functional>

#include "neighbour_store.hpp"
#include "src/simd/distance.hpp"

namespace vecdb {

// Forward declaration for snapshot support
class MmapStore;

// ──────────────────────────────────────────────────────────────────
//  Configuration for the HNSW index
// ──────────────────────────────────────────────────────────────────
struct HNSWConfig {
    int    dim             = 128;
    int    M               = 16;
    int    M_max0          = 32;
    int    ef_construction = 200;
    int    ef_search       = 50;
    int    max_layers      = 6;
    size_t arena_bytes     = 64ULL * 1024 * 1024;
};

// ──────────────────────────────────────────────────────────────────
//  One search result
// ──────────────────────────────────────────────────────────────────
struct HNSWSearchResult {
    float    dist;
    uint32_t node_id;
    bool operator>(const HNSWSearchResult& o) const { return dist > o.dist; }
    bool operator<(const HNSWSearchResult& o) const { return dist < o.dist; }
};

// ──────────────────────────────────────────────────────────────────
//  HNSWIndex
// ──────────────────────────────────────────────────────────────────
class HNSWIndex {
public:
    explicit HNSWIndex(const HNSWConfig& cfg);
    ~HNSWIndex() = default;

    // ── Mutating ──────────────────────────────────────────────────
    uint32_t insert(const float* vec);

    // ── Query ─────────────────────────────────────────────────────
    std::vector<HNSWSearchResult> search(const float* query, int top_k) const;
    std::vector<HNSWSearchResult> search(const float* query, int top_k, int ef) const;

    // ── Introspection ─────────────────────────────────────────────
    uint32_t size() const { return next_id_.load(std::memory_order_relaxed); }
    int      dim()  const { return cfg_.dim; }
    const HNSWConfig& config() const { return cfg_; }

    const float* get_vector(uint32_t node_id) const {
        return vectors_.data() + static_cast<size_t>(node_id) * cfg_.dim;
    }

    // ── Phase 4: snapshot support ──────────────────────────────────

    // Called by snapshot::save() — read-only accessors for the graph
    int      max_layer_stored()    const { return max_layer_.load(std::memory_order_relaxed); }
    uint32_t entry_point_stored()  const { return entry_point_.load(std::memory_order_relaxed); }
    int      layer_count_for(uint32_t node_id) const { return neighbours_.layer_count(node_id); }
    const std::pmr::vector<uint32_t>& neighbours_at(uint32_t node_id, int layer) const {
        return neighbours_.get_neighbours(node_id, layer);
    }

    // Called by snapshot::load() — restore full state from deserialized data
    void load_from_snapshot(
        const MmapStore& seg,
        const std::vector<std::vector<std::vector<uint32_t>>>& adj,
        uint32_t entry_point,
        int      max_layer);

private:
    using Candidates = std::vector<HNSWSearchResult>;

    int      sample_layer() const;
    uint32_t greedy_search_layer(uint32_t ep, const float* query, int layer) const;
    Candidates search_layer(uint32_t ep, const float* query, int ef, int layer) const;
    std::vector<uint32_t> select_neighbours(const Candidates& c, int M) const;
    void prune_connections(uint32_t node_id, int layer, int M_max);
    float l2_sq(const float* a, const float* b) const;

    HNSWConfig cfg_;
    alignas(64) std::vector<float> vectors_;
    NeighbourStore neighbours_;

    std::atomic<uint32_t> entry_point_{0};
    std::atomic<int>      max_layer_{-1};
    std::atomic<uint32_t> next_id_{0};

    mutable std::mt19937 rng_;
    mutable std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    mutable std::shared_mutex rwlock_;
};

} // namespace vecdb