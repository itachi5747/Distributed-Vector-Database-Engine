#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <atomic>

#include "consistent_hash.hpp"
#include "src/hnsw/index.hpp"
#include "src/wal/wal.hpp"
#include "src/storage/snapshot.hpp"

namespace vecdb {

struct ShardSearchResult {
    std::vector<HNSWSearchResult> results;
    bool     degraded   = false;
    uint32_t shards_ok  = 0;
    uint32_t shards_err = 0;
};

// Per-shard state — heap allocated (unique_ptr) so it is never moved/copied.
// atomic<bool> is not movable, so ShardState must stay at a fixed address.
struct ShardState {
    std::unique_ptr<HNSWIndex> index;
    std::unique_ptr<WAL>       wal;
    std::string                data_dir;
    uint32_t                   shard_id = 0;
    std::atomic<bool>          healthy{true};

    // Non-copyable, non-movable (atomic<bool> forbids both)
    ShardState() = default;
    ShardState(const ShardState&) = delete;
    ShardState& operator=(const ShardState&) = delete;
    ShardState(ShardState&&) = delete;
    ShardState& operator=(ShardState&&) = delete;
};

class ShardManager {
public:
    ShardManager(const HNSWConfig& cfg,
                 uint32_t shard_count,
                 const std::string& data_dir,
                 uint32_t virtual_nodes = 150);

    ~ShardManager() = default;

    // ── Write ─────────────────────────────────────────────────────────────────
    std::pair<uint32_t, uint32_t> insert(uint64_t    vector_id,
                                          const float* vec);

    // ── Read ──────────────────────────────────────────────────────────────────
    ShardSearchResult search(const float* query,
                              int          top_k,
                              int          ef = 0) const;

    // ── Persistence ───────────────────────────────────────────────────────────
    void snapshot_all();
    void load_all();

    // ── Introspection ─────────────────────────────────────────────────────────
    uint32_t shard_count()   const { return shard_count_; }
    uint64_t total_vectors() const;
    uint32_t shard_size(uint32_t shard_id) const;
    uint32_t owning_shard(uint64_t vector_id) const { return ring_.get_shard(vector_id); }
    std::vector<uint32_t> load_distribution() const;

    // Phase 6 accessors
    int  cfg_dim()                 const { return cfg_.dim; }
    bool shard_healthy(uint32_t s)  const { return s < shard_count_ && shards_[s]->healthy.load(); }

    // Exposed for testing degraded-flag behaviour
    std::vector<std::unique_ptr<ShardState>> shards_;

private:
    HNSWConfig         cfg_;
    uint32_t           shard_count_;
    std::string        data_dir_;
    ConsistentHashRing ring_;

    static std::vector<HNSWSearchResult>
    merge_results(std::vector<std::vector<HNSWSearchResult>>& partials, int top_k);
};

} // namespace vecdb
