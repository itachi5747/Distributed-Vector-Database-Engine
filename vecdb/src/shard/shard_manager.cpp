#include "shard_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <numeric>

namespace fs = std::filesystem;

namespace vecdb {

// ── Constructor ───────────────────────────────────────────────────────────────

ShardManager::ShardManager(const HNSWConfig& cfg,
                            uint32_t shard_count,
                            const std::string& data_dir,
                            uint32_t virtual_nodes)
    : cfg_(cfg)
    , shard_count_(shard_count)
    , data_dir_(data_dir)
    , ring_(shard_count, virtual_nodes)
{
    if (shard_count == 0)
        throw std::invalid_argument("ShardManager: shard_count must be > 0");

    fs::create_directories(data_dir_);

    // Allocate each shard on the heap (avoids move/copy of atomic<bool>)
    shards_.reserve(shard_count);
    for (uint32_t s = 0; s < shard_count; ++s) {
        auto ss       = std::make_unique<ShardState>();
        ss->shard_id  = s;
        ss->data_dir  = data_dir_;
        ss->healthy   = true;
        ss->index     = std::make_unique<HNSWIndex>(cfg_);

        std::string wal_path = data_dir_ + "/shard_" + std::to_string(s) + ".wal";
        ss->wal = std::make_unique<WAL>(wal_path,
                                        static_cast<uint32_t>(cfg_.dim),
                                        1000);
        shards_.push_back(std::move(ss));
    }
}

// ── insert ────────────────────────────────────────────────────────────────────

std::pair<uint32_t, uint32_t>
ShardManager::insert(uint64_t vector_id, const float* vec) {
    const uint32_t shard_id = ring_.get_shard(vector_id);
    ShardState& ss = *shards_[shard_id];

    ss.wal->append(vector_id, vec);
    uint32_t node_id = ss.index->insert(vec);
    return { shard_id, node_id };
}

// ── search — scatter-gather ───────────────────────────────────────────────────

ShardSearchResult
ShardManager::search(const float* query, int top_k, int ef) const {
    const int ef_actual = (ef > 0) ? ef : cfg_.ef_search;

    // Scatter: one async task per shard
    std::vector<std::future<std::vector<SearchResult>>> futures;
    futures.reserve(shard_count_);

    for (uint32_t s = 0; s < shard_count_; ++s) {
        const ShardState* ss = shards_[s].get();
        futures.push_back(
            std::async(std::launch::async,
                [ss, query, top_k, ef_actual]() -> std::vector<SearchResult> {
                    if (!ss->healthy.load(std::memory_order_relaxed)) return {};
                    return ss->index->search(query, top_k, ef_actual);
                })
        );
    }

    // Gather
    std::vector<std::vector<SearchResult>> partials;
    partials.reserve(shard_count_);

    ShardSearchResult result;
    for (uint32_t s = 0; s < shard_count_; ++s) {
        try {
            partials.push_back(futures[s].get());
            ++result.shards_ok;
        } catch (...) {
            shards_[s]->healthy.store(false, std::memory_order_relaxed);
            ++result.shards_err;
            result.degraded = true;
            partials.push_back({});
        }
    }

    result.results = merge_results(partials, top_k);
    return result;
}

// ── merge_results ─────────────────────────────────────────────────────────────

std::vector<SearchResult>
ShardManager::merge_results(std::vector<std::vector<SearchResult>>& partials,
                              int top_k) {
    size_t total = 0;
    for (auto& p : partials) total += p.size();
    if (total == 0) return {};

    std::vector<SearchResult> all;
    all.reserve(total);
    for (auto& p : partials)
        for (auto& r : p)
            all.push_back(r);

    int k = std::min(static_cast<int>(all.size()), top_k);
    std::partial_sort(all.begin(), all.begin() + k, all.end());
    all.resize(k);
    return all;
}

// ── snapshot_all ──────────────────────────────────────────────────────────────

void ShardManager::snapshot_all() {
    for (uint32_t s = 0; s < shard_count_; ++s) {
        ShardState& ss = *shards_[s];
        auto paths = storage::make_paths(data_dir_, s);
        storage::save_and_truncate_wal(*ss.index, paths, *ss.wal);
    }
}

// ── load_all ──────────────────────────────────────────────────────────────────

void ShardManager::load_all() {
    for (uint32_t s = 0; s < shard_count_; ++s) {
        ShardState& ss = *shards_[s];
        auto paths = storage::make_paths(data_dir_, s);
        if (!fs::exists(paths.seg) || !fs::exists(paths.graph)) continue;
        storage::load(*ss.index, paths);
        ss.wal->replay([&ss](const WALEntry& e) {
            ss.index->insert(e.vec);
        });
    }
}

// ── total_vectors ─────────────────────────────────────────────────────────────

uint64_t ShardManager::total_vectors() const {
    uint64_t total = 0;
    for (const auto& ss : shards_) total += ss->index->size();
    return total;
}

// ── shard_size ────────────────────────────────────────────────────────────────

uint32_t ShardManager::shard_size(uint32_t shard_id) const {
    if (shard_id >= shard_count_)
        throw std::out_of_range("ShardManager::shard_size: invalid shard_id");
    return shards_[shard_id]->index->size();
}

// ── load_distribution ────────────────────────────────────────────────────────

std::vector<uint32_t> ShardManager::load_distribution() const {
    std::vector<uint32_t> dist(shard_count_);
    for (uint32_t s = 0; s < shard_count_; ++s)
        dist[s] = shards_[s]->index->size();
    return dist;
}

} // namespace vecdb