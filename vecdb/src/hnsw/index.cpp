#include "index.hpp"
#include "src/storage/mmap_store.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>

namespace vecdb {

// ────────────────────────────────────────────────────────────────────────────
//  Constructor
// ────────────────────────────────────────────────────────────────────────────
HNSWIndex::HNSWIndex(const HNSWConfig& cfg)
    : cfg_(cfg)
    , neighbours_(cfg.arena_bytes)
    , rng_(std::random_device{}())
{
    if (cfg_.dim <= 0)              throw std::invalid_argument("dim must be > 0");
    if (cfg_.M < 2)                 throw std::invalid_argument("M must be >= 2");
    if (cfg_.ef_construction < 1)   throw std::invalid_argument("ef_construction must be >= 1");
    if (cfg_.max_layers < 1)        throw std::invalid_argument("max_layers must be >= 1");

    // Auto-initialise SIMD dispatch on first index construction.
    // safe to call multiple times — subsequent calls are no-ops.
    vecdb::simd::init();

    // Pre-reserve some vector storage (avoids realloc on early inserts)
    vectors_.reserve(static_cast<size_t>(1024) * cfg_.dim);
}

// ────────────────────────────────────────────────────────────────────────────
//  Distance kernel — SIMD-dispatched (Phase 2)
//
//  Routes to AVX-512 / AVX2 / scalar via function pointer set by simd::init().
//  Zero branch overhead on the hot path — the pointer is resolved once at startup.
// ────────────────────────────────────────────────────────────────────────────
float HNSWIndex::l2_sq(const float* a, const float* b) const {
    return vecdb::simd::l2_sq(a, b, cfg_.dim);
}

// ────────────────────────────────────────────────────────────────────────────
//  sample_layer
//
//  HNSW paper equation: l = floor(-ln(uniform(0,1)) * M_L)
//  where M_L = 1 / ln(M) is the level multiplier.
//  Higher M → sparser upper layers → faster search.
// ────────────────────────────────────────────────────────────────────────────
int HNSWIndex::sample_layer() const {
    // M_L = 1 / ln(M)
    const double M_L = 1.0 / std::log(static_cast<double>(cfg_.M));
    const double r   = uniform_(rng_);
    // Clamp to [0, max_layers - 1]
    int l = static_cast<int>(-std::log(r + 1e-15) * M_L);
    return std::min(l, cfg_.max_layers - 1);
}

// ────────────────────────────────────────────────────────────────────────────
//  greedy_search_layer (ef = 1 coarse descent)
//
//  Used during insert to navigate from the top layer down to (target_layer+1).
//  We only need the single closest node — no beam needed.
// ────────────────────────────────────────────────────────────────────────────
uint32_t HNSWIndex::greedy_search_layer(uint32_t ep,
                                         const float* query,
                                         int target_layer) const {
    uint32_t current = ep;
    float    current_dist = l2_sq(get_vector(current), query);

    bool improved = true;
    while (improved) {
        improved = false;
        const auto& nbrs = neighbours_.get_neighbours(current, target_layer);
        for (uint32_t nbr : nbrs) {
            float d = l2_sq(get_vector(nbr), query);
            if (d < current_dist) {
                current_dist = d;
                current      = nbr;
                improved     = true;
            }
        }
    }
    return current;
}

// ────────────────────────────────────────────────────────────────────────────
//  search_layer — beam search at a single layer
//
//  Implements HNSW paper Algorithm 2 (SELECT-NEIGHBOURS is separate).
//  Returns up to `ef` candidates sorted closest-first.
//
//  Data structures:
//    candidates — min-heap (closest candidate at top) we expand from
//    results    — max-heap (furthest result at top) we prune to ef entries
//    visited    — flat bool array to avoid revisiting nodes
// ────────────────────────────────────────────────────────────────────────────
HNSWIndex::Candidates
HNSWIndex::search_layer(uint32_t ep,
                         const float* query,
                         int ef,
                         int layer) const {
    const uint32_t n = next_id_.load(std::memory_order_relaxed);

    // visited[] marks nodes already popped from the candidate heap
    std::vector<bool> visited(n, false);

    // candidates: min-heap — we expand the closest unexplored node first
    // Using HNSWSearchResult with operator> for min-heap behaviour in priority_queue
    using MinHeap = std::priority_queue<HNSWSearchResult,
                                        std::vector<HNSWSearchResult>,
                                        std::greater<HNSWSearchResult>>;
    // results: max-heap — we keep the ef closest results found so far
    using MaxHeap = std::priority_queue<HNSWSearchResult>;

    float ep_dist = l2_sq(get_vector(ep), query);
    visited[ep] = true;

    MinHeap candidates;
    MaxHeap results;
    candidates.push({ep_dist, ep});
    results.push({ep_dist, ep});

    while (!candidates.empty()) {
        HNSWSearchResult c = candidates.top();
        candidates.pop();

        // Early termination: if the closest candidate is further than the
        // furthest result we already have (and we have ef results), stop.
        if (results.size() >= static_cast<size_t>(ef) &&
            c.dist > results.top().dist) {
            break;
        }

        // Expand neighbours of c
        const auto& nbrs = neighbours_.get_neighbours(c.node_id, layer);
        for (uint32_t nbr : nbrs) {
            if (nbr >= n || visited[nbr]) continue;
            visited[nbr] = true;

            float d = l2_sq(get_vector(nbr), query);

            // Add to results if closer than worst result, or results not full
            if (results.size() < static_cast<size_t>(ef) ||
                d < results.top().dist) {
                candidates.push({d, nbr});
                results.push({d, nbr});
                // Keep results heap bounded to ef entries
                if (results.size() > static_cast<size_t>(ef)) {
                    results.pop();
                }
            }
        }
    }

    // Flatten results into a vector, sorted closest-first
    Candidates out;
    out.reserve(results.size());
    while (!results.empty()) {
        out.push_back(results.top());
        results.pop();
    }
    std::sort(out.begin(), out.end()); // ascending by dist
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  select_neighbours
//
//  Simple greedy heuristic: take the M closest candidates.
//  (Full HNSW paper uses a more complex heuristic that improves graph
//  connectivity; the simple version is correct and ~95% as good for recall.)
// ────────────────────────────────────────────────────────────────────────────
std::vector<uint32_t>
HNSWIndex::select_neighbours(const Candidates& candidates, int M) const {
    std::vector<uint32_t> selected;
    const int take = std::min(M, static_cast<int>(candidates.size()));
    selected.reserve(take);
    // candidates is sorted closest-first
    for (int i = 0; i < take; ++i) {
        selected.push_back(candidates[i].node_id);
    }
    return selected;
}

// ────────────────────────────────────────────────────────────────────────────
//  prune_connections
//
//  If a node's neighbour list exceeds M_max, trim it to the M_max closest.
// ────────────────────────────────────────────────────────────────────────────
void HNSWIndex::prune_connections(uint32_t node_id, int layer, int M_max) {
    const auto& nbrs = neighbours_.get_neighbours(node_id, layer);
    if (static_cast<int>(nbrs.size()) <= M_max) return;

    // Sort by distance to node_id, keep M_max closest
    const float* v = get_vector(node_id);
    std::vector<std::pair<float, uint32_t>> dist_nbr;
    dist_nbr.reserve(nbrs.size());
    for (uint32_t n : nbrs) {
        dist_nbr.push_back({l2_sq(v, get_vector(n)), n});
    }
    std::sort(dist_nbr.begin(), dist_nbr.end()); // ascending by distance

    std::vector<uint32_t> pruned;
    pruned.reserve(M_max);
    for (int i = 0; i < M_max; ++i) pruned.push_back(dist_nbr[i].second);

    neighbours_.set_neighbours(node_id, layer, pruned);
}

// ────────────────────────────────────────────────────────────────────────────
//  insert  (HNSW paper Algorithm 1)
// ────────────────────────────────────────────────────────────────────────────
uint32_t HNSWIndex::insert(const float* vec) {
    // ── Acquire exclusive lock (covers vectors_, neighbours_, atomics) ──────
    std::unique_lock<std::shared_mutex> lock(rwlock_);

    // 1. Assign node id and copy vector data
    const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    const size_t offset = static_cast<size_t>(id) * cfg_.dim;

    // Grow vector storage
    vectors_.resize(offset + cfg_.dim);
    std::copy(vec, vec + cfg_.dim, vectors_.begin() + offset);

    // Grow adjacency store (+1 node, max_layers slots)
    neighbours_.resize(id + 1, cfg_.max_layers);

    // 2. Sample the layer for this new node
    const int node_layer = sample_layer();

    // 3. If index is empty, this node becomes the entry point
    const int cur_max = max_layer_.load(std::memory_order_relaxed);
    if (cur_max < 0) {
        entry_point_.store(id, std::memory_order_relaxed);
        max_layer_.store(node_layer, std::memory_order_relaxed);
        return id;
    }

    uint32_t ep = entry_point_.load(std::memory_order_relaxed);

    // 4. Greedy descent from top layer down to node_layer + 1 (ef = 1)
    for (int l = cur_max; l > node_layer; --l) {
        ep = greedy_search_layer(ep, vec, l);
    }

    // 5. Layer-by-layer beam search from node_layer down to 0
    for (int l = std::min(node_layer, cur_max); l >= 0; --l) {
        Candidates candidates = search_layer(ep, vec, cfg_.ef_construction, l);

        // Pick M (or M_max0 at layer 0) neighbours for the new node
        const int M_cur = (l == 0) ? cfg_.M_max0 : cfg_.M;
        auto selected = select_neighbours(candidates, M_cur);

        // Connect new node → selected neighbours
        neighbours_.set_neighbours(id, l, selected);

        // Connect selected neighbours → new node (bidirectional)
        const int M_max = (l == 0) ? cfg_.M_max0 : cfg_.M;
        for (uint32_t nbr : selected) {
            neighbours_.add_neighbour(nbr, l, id);
            // Prune if over limit
            prune_connections(nbr, l, M_max);
        }

        // Update entry point for the next (lower) layer
        if (!candidates.empty()) ep = candidates[0].node_id;
    }

    // 6. Update global entry point if this node reaches a new top layer
    if (node_layer > cur_max) {
        entry_point_.store(id,         std::memory_order_relaxed);
        max_layer_.store(node_layer,   std::memory_order_relaxed);
    }

    return id;
}

// ────────────────────────────────────────────────────────────────────────────
//  search  (HNSW paper Algorithm 5)
// ────────────────────────────────────────────────────────────────────────────
std::vector<HNSWSearchResult>
HNSWIndex::search(const float* query, int top_k) const {
    return search(query, top_k, cfg_.ef_search);
}

std::vector<HNSWSearchResult>
HNSWIndex::search(const float* query, int top_k, int ef) const {
    // ── Shared lock — concurrent searches run truly in parallel ──────────
    std::shared_lock<std::shared_mutex> lock(rwlock_);

    const int cur_max = max_layer_.load(std::memory_order_relaxed);
    if (cur_max < 0) return {}; // empty index

    // Ensure ef is at least top_k so we can return top_k results
    const int ef_actual = std::max(ef, top_k);

    uint32_t ep = entry_point_.load(std::memory_order_relaxed);

    // 1. Coarse greedy descent from top layer down to layer 1 (ef = 1)
    for (int l = cur_max; l > 0; --l) {
        ep = greedy_search_layer(ep, query, l);
    }

    // 2. Beam search at layer 0 with ef_actual candidates
    Candidates candidates = search_layer(ep, query, ef_actual, 0);

    // 3. Return top_k
    if (static_cast<int>(candidates.size()) > top_k) {
        candidates.resize(top_k);
    }
    return candidates;
}

} // namespace vecdb


namespace vecdb {

// ────────────────────────────────────────────────────────────────────────────
//  load_from_snapshot  (Phase 4)
//
//  Restores the complete index state from a MmapStore (vectors) and a
//  deserialized adjacency list (graph). Called by snapshot::load().
//
//  Must be called on a freshly constructed, empty index.
// ────────────────────────────────────────────────────────────────────────────
void HNSWIndex::load_from_snapshot(
    const MmapStore& seg,
    const std::vector<std::vector<std::vector<uint32_t>>>& adj,
    uint32_t entry_point,
    int      max_layer)
{
    std::unique_lock<std::shared_mutex> lock(rwlock_);

    const uint32_t N = seg.vec_count();

    // ── 1. Copy vectors from segment into our float array ─────────────────────
    vectors_.resize(static_cast<size_t>(N) * cfg_.dim);
    for (uint32_t i = 0; i < N; ++i) {
        const float* src = seg.get(i);
        float* dst = vectors_.data() + static_cast<size_t>(i) * cfg_.dim;
        std::copy(src, src + cfg_.dim, dst);
    }

    // ── 2. Restore adjacency lists ────────────────────────────────────────────
    neighbours_.resize(N, cfg_.max_layers);
    for (uint32_t node = 0; node < N && node < adj.size(); ++node) {
        for (int l = 0; l < static_cast<int>(adj[node].size()); ++l) {
            neighbours_.set_neighbours(node, l, adj[node][l]);
        }
    }

    // ── 3. Restore metadata ───────────────────────────────────────────────────
    next_id_.store(N, std::memory_order_relaxed);
    entry_point_.store(entry_point, std::memory_order_relaxed);
    max_layer_.store(max_layer, std::memory_order_relaxed);
}

} // namespace vecdb (load_from_snapshot)