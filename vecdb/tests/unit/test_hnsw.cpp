// Phase 1 unit tests for HNSWIndex
//
// Tests:
//   1. Basic insert/search sanity (small dataset)
//   2. Recall@10 >= 90% against brute force on 10K random 64-dim vectors
//   3. Concurrent read correctness (shared_mutex)
//   4. Distance correctness vs scalar kernel

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "src/hnsw/index.hpp"
#include "src/simd/distance.hpp"
#include "src/simd/distance.hpp"

#include <vector>
#include <random>
#include <algorithm>
#include <thread>
#include <numeric>
#include <cmath>

// ──────────────────────────────────────────────────────────────────
//  Helpers
// ──────────────────────────────────────────────────────────────────

// Generate N random unit vectors of `dim` floats
static std::vector<std::vector<float>>
make_random_vectors(int N, int dim, uint64_t seed = 42) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    std::vector<std::vector<float>> vecs(N, std::vector<float>(dim));
    for (auto& v : vecs) {
        float norm = 0.0f;
        for (float& x : v) { x = dist(rng); norm += x * x; }
        norm = std::sqrt(norm);
        for (float& x : v) x /= (norm + 1e-9f);
    }
    return vecs;
}

// Brute-force exact top-K nearest neighbours (by squared L2)
static std::vector<uint32_t>
brute_force_topk(const std::vector<std::vector<float>>& dataset,
                 const std::vector<float>& query,
                 int top_k) {
    const int dim = static_cast<int>(query.size());
    std::vector<std::pair<float, uint32_t>> dists;
    dists.reserve(dataset.size());
    for (uint32_t i = 0; i < dataset.size(); ++i) {
        float d = vecdb::simd::l2_sq_scalar(dataset[i].data(), query.data(), dim);
        dists.push_back({d, i});
    }
    std::partial_sort(dists.begin(),
                      dists.begin() + std::min(top_k, (int)dists.size()),
                      dists.end());
    std::vector<uint32_t> ids;
    for (int i = 0; i < std::min(top_k, (int)dists.size()); ++i)
        ids.push_back(dists[i].second);
    return ids;
}

// Compute recall@K: fraction of true top-K found in HNSW top-K results
static float recall_at_k(const std::vector<uint32_t>& truth,
                          const std::vector<vecdb::HNSWSearchResult>& found) {
    if (truth.empty()) return 1.0f;
    int hits = 0;
    for (auto& r : found) {
        for (uint32_t t : truth) {
            if (r.node_id == t) { ++hits; break; }
        }
    }
    return static_cast<float>(hits) / static_cast<float>(truth.size());
}

// ──────────────────────────────────────────────────────────────────
//  Test 1: sanity — insert 5 vectors, search returns them
// ──────────────────────────────────────────────────────────────────
TEST_CASE("insert and search basic sanity") {
    vecdb::HNSWConfig cfg;
    cfg.dim              = 4;
    cfg.M                = 4;
    cfg.M_max0           = 8;
    cfg.ef_construction  = 10;
    cfg.ef_search        = 10;
    cfg.max_layers       = 4;
    cfg.arena_bytes      = 4ULL * 1024 * 1024;

    vecdb::HNSWIndex idx(cfg);

    // Insert 5 known vectors
    std::vector<std::vector<float>> vecs = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, 0.5f, 0.0f, 0.0f},
    };
    for (const auto& v : vecs) idx.insert(v.data());

    CHECK(idx.size() == 5);

    // Query closest to [1, 0, 0, 0] — should be node 0
    std::vector<float> query = {1.0f, 0.0f, 0.0f, 0.0f};
    auto results = idx.search(query.data(), 1);

    REQUIRE(!results.empty());
    CHECK(results[0].node_id == 0u);
    CHECK(results[0].dist < 1e-6f);
}

// ──────────────────────────────────────────────────────────────────
//  Test 2: recall@10 >= 90% on 10K random 64-dim vectors
// ──────────────────────────────────────────────────────────────────
TEST_CASE("recall@10 >= 90% on 10K vectors") {
    vecdb::simd::init(); // ensure dispatch is set before index construction
    const int N   = 10'000;
    const int DIM = 64;
    const int K   = 10;

    vecdb::HNSWConfig cfg;
    cfg.dim              = DIM;
    cfg.M                = 16;
    cfg.M_max0           = 32;
    cfg.ef_construction  = 200;
    cfg.ef_search        = 100;
    cfg.max_layers       = 6;
    cfg.arena_bytes      = 64ULL * 1024 * 1024;

    vecdb::HNSWIndex idx(cfg);

    auto dataset = make_random_vectors(N, DIM, /*seed=*/42);
    for (const auto& v : dataset) idx.insert(v.data());

    CHECK(idx.size() == static_cast<uint32_t>(N));

    // Run 100 random queries, compute average recall
    auto queries = make_random_vectors(100, DIM, /*seed=*/99);
    double total_recall = 0.0;

    for (const auto& q : queries) {
        auto truth   = brute_force_topk(dataset, q, K);
        auto results = idx.search(q.data(), K);
        total_recall += recall_at_k(truth, results);
    }

    double avg_recall = total_recall / queries.size();
    MESSAGE("Average Recall@10 = " << avg_recall);
    CHECK(avg_recall >= 0.90);
}

// ──────────────────────────────────────────────────────────────────
//  Test 3: concurrent searches don't race or corrupt state
// ──────────────────────────────────────────────────────────────────
TEST_CASE("concurrent searches are safe") {
    const int N   = 2000;
    const int DIM = 32;

    vecdb::HNSWConfig cfg;
    cfg.dim              = DIM;
    cfg.M                = 12;
    cfg.M_max0           = 24;
    cfg.ef_construction  = 100;
    cfg.ef_search        = 50;
    cfg.max_layers       = 5;
    cfg.arena_bytes      = 16ULL * 1024 * 1024;

    vecdb::HNSWIndex idx(cfg);

    auto dataset = make_random_vectors(N, DIM, 7);
    for (const auto& v : dataset) idx.insert(v.data());

    std::vector<float> query(DIM, 0.1f);
    const int THREADS = 8;
    const int QUERIES_PER_THREAD = 200;

    std::vector<std::thread> threads;
    std::atomic<int> total_results{0};

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int q = 0; q < QUERIES_PER_THREAD; ++q) {
                auto results = idx.search(query.data(), 5);
                total_results += static_cast<int>(results.size());
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(total_results == THREADS * QUERIES_PER_THREAD * 5);
}

// ──────────────────────────────────────────────────────────────────
//  Test 4: l2_sq_scalar correctness
// ──────────────────────────────────────────────────────────────────
TEST_CASE("l2_sq_scalar correctness") {
    // ||[1,0,0] - [0,1,0]||² = 2
    float a[] = {1.0f, 0.0f, 0.0f};
    float b[] = {0.0f, 1.0f, 0.0f};
    CHECK(vecdb::simd::l2_sq_scalar(a, b, 3) == doctest::Approx(2.0f).epsilon(1e-6f));

    // Same vector → 0
    CHECK(vecdb::simd::l2_sq_scalar(a, a, 3) == doctest::Approx(0.0f).epsilon(1e-9f));
}

// ──────────────────────────────────────────────────────────────────
//  Test 5: cosine_scalar correctness
// ──────────────────────────────────────────────────────────────────
TEST_CASE("cosine_scalar correctness") {
    float a[] = {1.0f, 0.0f};
    float b[] = {0.0f, 1.0f};
    // Orthogonal vectors → cosine distance = 1
    CHECK(vecdb::simd::cosine_scalar(a, b, 2) == doctest::Approx(1.0f).epsilon(1e-6f));

    // Same direction → 0
    float c[] = {2.0f, 0.0f};
    CHECK(vecdb::simd::cosine_scalar(a, c, 2) == doctest::Approx(0.0f).epsilon(1e-6f));
}

// ──────────────────────────────────────────────────────────────────
//  Test 6: index grows correctly across many inserts
// ──────────────────────────────────────────────────────────────────
TEST_CASE("size tracks inserts correctly") {
    vecdb::HNSWConfig cfg;
    cfg.dim    = 8;
    cfg.M      = 8;
    cfg.M_max0 = 16;
    cfg.ef_construction = 50;
    cfg.arena_bytes = 8ULL * 1024 * 1024;

    vecdb::HNSWIndex idx(cfg);
    std::vector<float> v(8, 0.5f);

    for (int i = 1; i <= 500; ++i) {
        v[0] = static_cast<float>(i) / 500.0f;
        idx.insert(v.data());
        CHECK(idx.size() == static_cast<uint32_t>(i));
    }
}