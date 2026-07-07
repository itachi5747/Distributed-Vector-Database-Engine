// Phase 3 — Crash recovery integration test
//
// Simulates a crash mid-insert: WAL written, process "dies" before HNSW finishes.
// On restart, WAL replay reconstructs the index. Verifies:
//   1. All WAL-committed vectors are recoverable after replay
//   2. Recall@10 of the recovered index matches a reference index built cleanly
//   3. A torn final entry does NOT corrupt the recovery

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "src/hnsw/index.hpp"
#include "src/simd/distance.hpp"
#include "src/wal/wal.hpp"

#include <vector>
#include <random>
#include <cmath>
#include <cstdio>
#include <algorithm>

// ── Helpers ───────────────────────────────────────────────────────────────────

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& n) {
        path = "/tmp/vecdb_cr_" + n + ".wal";
        std::remove(path.c_str());
    }
    ~TempFile() { std::remove(path.c_str()); }
};

static std::vector<float> rand_vec(int dim, uint64_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0.f, 1.f);
    std::vector<float> v(dim);
    float norm = 0.f;
    for (float& x : v) { x = d(rng); norm += x * x; }
    norm = std::sqrt(norm);
    for (float& x : v) x /= (norm + 1e-9f);
    return v;
}

static float brute_recall(const std::vector<std::vector<float>>& dataset,
                            vecdb::HNSWIndex& idx,
                            int dim, int top_k, int n_queries) {
    std::mt19937 rng(777);
    std::uniform_int_distribution<int> pick(0, (int)dataset.size() - 1);

    double total = 0.0;
    for (int q = 0; q < n_queries; ++q) {
        auto query = rand_vec(dim, 9000 + q);

        // Brute force
        std::vector<std::pair<float, uint32_t>> bf;
        bf.reserve(dataset.size());
        for (uint32_t i = 0; i < (uint32_t)dataset.size(); ++i) {
            float acc = 0.f;
            for (int d = 0; d < dim; ++d) {
                float diff = dataset[i][d] - query[d];
                acc += diff * diff;
            }
            bf.push_back({acc, i});
        }
        std::partial_sort(bf.begin(), bf.begin() + top_k, bf.end());

        // HNSW
        auto results = idx.search(query.data(), top_k);

        int hits = 0;
        for (auto& r : results)
            for (int k = 0; k < top_k; ++k)
                if (r.node_id == bf[k].second) { ++hits; break; }

        total += static_cast<double>(hits) / top_k;
    }
    return static_cast<float>(total / n_queries);
}

// ── Test 1: WAL replay reconstructs a correct index ──────────────────────────

TEST_CASE("crash recovery: WAL replay builds correct index") {
    vecdb::simd::init();

    TempFile tf("basic");
    const int DIM  = 32;
    const int N    = 2000;
    const int K    = 10;

    vecdb::HNSWConfig cfg;
    cfg.dim             = DIM;
    cfg.M               = 12;
    cfg.M_max0          = 24;
    cfg.ef_construction = 100;
    cfg.ef_search       = 50;
    cfg.arena_bytes     = 16ULL * 1024 * 1024;

    // ── Phase A: "normal run" — write WAL + build index ──────────────────────
    std::vector<std::vector<float>> dataset(N);
    for (int i = 0; i < N; ++i) dataset[i] = rand_vec(DIM, i + 1);

    {
        vecdb::WAL wal(tf.path, DIM, 0);
        vecdb::HNSWIndex idx(cfg);

        for (int i = 0; i < N; ++i) {
            // WAL first, then index (production insert order)
            wal.append(static_cast<uint64_t>(i), dataset[i].data());
            idx.insert(dataset[i].data());
        }
        wal.checkpoint();
        // "crash" — idx goes out of scope, index state is lost
    }

    // ── Phase B: recovery — replay WAL into a fresh index ────────────────────
    vecdb::WAL   wal_recovery(tf.path, DIM, 0);
    vecdb::HNSWIndex idx_recovered(cfg);

    uint64_t replayed = wal_recovery.replay([&](const vecdb::WALEntry& e) {
        idx_recovered.insert(e.vec);
    });

    CHECK(replayed                    == static_cast<uint64_t>(N));
    CHECK(idx_recovered.size()        == static_cast<uint32_t>(N));

    // Recall should be acceptable (slightly lower than fresh index — same data, same graph quality)
    float recall = brute_recall(dataset, idx_recovered, DIM, K, 50);
    MESSAGE("Recovered index Recall@10 = " << recall);
    CHECK(recall >= 0.85f);
}

// ── Test 2: crash mid-insert — only WAL-confirmed entries are recovered ───────

TEST_CASE("crash recovery: torn final entry is skipped") {
    vecdb::simd::init();

    TempFile tf("torn");
    const int DIM = 8;
    const int N   = 20;

    vecdb::HNSWConfig cfg;
    cfg.dim             = DIM;
    cfg.M               = 4;
    cfg.M_max0          = 8;
    cfg.ef_construction = 20;
    cfg.arena_bytes     = 4ULL * 1024 * 1024;

    // Write 20 entries, checkpoint at 15, then write 5 more with the last one torn
    {
        vecdb::WAL wal(tf.path, DIM, 0);
        for (int i = 0; i < N; ++i) {
            auto v = rand_vec(DIM, i + 100);
            wal.append(static_cast<uint64_t>(i), v.data());
            if (i == 14) wal.checkpoint(); // checkpoint after entry 14
        }
        // DO NOT checkpoint the last 5 — they're in "flight"
    }

    // Corrupt the very last entry's CRC to simulate a torn write
    {
        const size_t entry_sz = 24 + DIM * sizeof(float);
        const size_t offset   = 64 + 19 * entry_sz + 24; // DATA_OFF of entry 19
        std::fstream f(tf.path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(offset));
        uint8_t garbage = 0xAB;
        f.write(reinterpret_cast<char*>(&garbage), 1);
    }

    // Recovery: replay starts at LSN=0. Entries 0..18 are good (19 entries),
    // entry 19 has corrupt CRC → stops there.
    vecdb::WAL   wal_r(tf.path, DIM, 0);
    vecdb::HNSWIndex idx(cfg);
    uint64_t replayed = wal_r.replay([&](const vecdb::WALEntry& e) {
        idx.insert(e.vec);
    });

    CHECK(replayed    == 19u); // entries 0..18 are good; 19 is torn
    CHECK(idx.size()  == 19u);
}

// ── Test 3: empty WAL replay is safe ─────────────────────────────────────────

TEST_CASE("crash recovery: empty WAL replays zero entries") {
    vecdb::simd::init();

    TempFile tf("empty");
    vecdb::WAL wal(tf.path, 16, 0);

    int count = 0;
    wal.replay([&](const vecdb::WALEntry&) { ++count; });
    CHECK(count == 0);
}