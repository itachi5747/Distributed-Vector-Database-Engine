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

// ──────────────────────────────────────────────
//  Configuration for the HNSW index
// ──────────────────────────────────────────────
struct HNSWConfig {
    int    dim             = 128;   // vector dimensionality
    int    M               = 16;    // max neighbours per node per layer
    int    M_max0          = 32;    // max neighbours at layer 0 (usually 2*M)
    int    ef_construction = 200;   // beam width during build
    int    ef_search       = 50;    // beam width during query
    int    max_layers      = 6;     // hard cap on layer height
    size_t arena_bytes     = 64ULL * 1024 * 1024; // 64 MB arena for neighbour store
};

// ──────────────────────────────────────────────
//  One search result: (distance², node_id)
// ──────────────────────────────────────────────
struct SearchResult {
    float    dist;    // squared L2 distance (or cosine distance)
    uint32_t node_id;

    // min-heap ordering (smallest dist first)
    bool operator>(const SearchResult& o) const { return dist > o.dist; }
    bool operator<(const SearchResult& o) const { return dist < o.dist; }
};

// ──────────────────────────────────────────────
//  HNSWIndex
//
//  Single-shard, single-process HNSW index.
//  Thread-safe: concurrent searches, exclusive inserts.
// ──────────────────────────────────────────────
class HNSWIndex {
public:
    explicit HNSWIndex(const HNSWConfig& cfg);
    ~HNSWIndex() = default;

    // ── Mutating operations ──────────────────
    // Insert a vector; returns its internal node_id.
    // `vec` must point to cfg.dim floats.
    uint32_t insert(const float* vec);

    // ── Query operations ─────────────────────
    // Return top-K nearest neighbours of `query`.
    // `query` must point to cfg.dim floats.
    std::vector<SearchResult> search(const float* query, int top_k) const;
    std::vector<SearchResult> search(const float* query, int top_k, int ef) const;

    // ── Introspection ────────────────────────
    uint32_t size() const { return next_id_.load(std::memory_order_relaxed); }
    int      dim()  const { return cfg_.dim; }
    const HNSWConfig& config() const { return cfg_; }

    // Raw vector access (read-only, no bounds check)
    const float* get_vector(uint32_t node_id) const {
        return vectors_.data() + static_cast<size_t>(node_id) * cfg_.dim;
    }

private:
    // ── Internal types ───────────────────────
    using Candidates = std::vector<SearchResult>; // max-heap by dist

    // ── Core HNSW algorithms ─────────────────

    // Sample the layer height for a new node (Algorithm 1, step 1)
    int  sample_layer() const;

    // Greedy search from ep down to target_layer, ef=1 (coarse descent)
    // Returns the single closest node found at target_layer.
    uint32_t greedy_search_layer(uint32_t ep,
                                  const float* query,
                                  int target_layer) const;

    // Beam search at a single layer.
    // Returns up to ef candidates sorted by distance (closest first).
    Candidates search_layer(uint32_t ep,
                             const float* query,
                             int ef,
                             int layer) const;

    // Select best M neighbours from candidates (simple heuristic: take M closest)
    std::vector<uint32_t> select_neighbours(const Candidates& candidates,
                                             int M) const;

    // Prune neighbour list to at most M_max entries
    void prune_connections(uint32_t node_id, int layer, int M_max);

    // ── Distance kernel ──────────────────────
    float l2_sq(const float* a, const float* b) const;

    // ── Data members ─────────────────────────
    HNSWConfig cfg_;

    // Flat vector storage: vector i lives at vectors_[i * dim]
    // Aligned to 64 bytes for cache-line efficiency
    alignas(64) std::vector<float> vectors_;

    // Adjacency lists (arena-allocated)
    NeighbourStore neighbours_;

    // Global entry point and current max layer
    std::atomic<uint32_t> entry_point_{0};
    std::atomic<int>      max_layer_{-1};  // -1 = index empty

    // Next available node id
    std::atomic<uint32_t> next_id_{0};

    // RNG for layer sampling (mutable so search can be const; protected by write lock)
    mutable std::mt19937 rng_;
    mutable std::uniform_real_distribution<double> uniform_{0.0, 1.0};

    // Read-write lock: shared for search, exclusive for insert
    mutable std::shared_mutex rwlock_;
};

} // namespace vecdb
