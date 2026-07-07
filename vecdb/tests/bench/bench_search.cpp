// Phase 2 — Throughput benchmark
//
// Measures:
//   1. Distance kernel throughput (GFLOPs/s) for each ISA level
//   2. HNSW search throughput (queries/second) at dim=768
//   3. HNSW insert throughput (inserts/second)
//
// Build: cmake --build build --target bench_search
// Run:   ./build/bench_search

#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <string>

#include "src/simd/distance.hpp"
#include "src/hnsw/index.hpp"

using Clock = std::chrono::high_resolution_clock;
using Dur   = std::chrono::duration<double>;

// ── Random data helpers ────────────────────────────────────────────────────────

static std::vector<float> rand_vec(int dim, uint64_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0.f, 1.f);
    std::vector<float> v(dim);
    float norm = 0.f;
    for (float& x : v) { x = d(rng); norm += x*x; }
    norm = std::sqrt(norm);
    for (float& x : v) x /= norm;
    return v;
}

static std::vector<std::vector<float>> rand_matrix(int N, int dim, uint64_t seed=42) {
    std::vector<std::vector<float>> m(N);
    for (int i = 0; i < N; ++i) m[i] = rand_vec(dim, seed + i);
    return m;
}

// ── Benchmark runner ──────────────────────────────────────────────────────────

struct Result {
    std::string name;
    double ops_per_sec;
    double gflops;
};

// Run `fn` for `duration_s` seconds, return ops/sec
template<typename Fn>
static double measure(Fn fn, double duration_s = 2.0) {
    auto start = Clock::now();
    long long iters = 0;
    while (Dur(Clock::now() - start).count() < duration_s) {
        fn();
        ++iters;
    }
    double elapsed = Dur(Clock::now() - start).count();
    return static_cast<double>(iters) / elapsed;
}

// ── Distance kernel benchmarks ────────────────────────────────────────────────

void bench_distance(int dim) {
    auto a = rand_vec(dim, 1);
    auto b = rand_vec(dim, 2);
    const double flops_per_call = 2.0 * dim; // dim multiplies + dim adds

    std::cout << "\n── Distance kernel throughput  (dim=" << dim << ") ──────────────────\n";
    std::cout << std::left << std::setw(20) << "Kernel"
              << std::right << std::setw(16) << "Mcalls/s"
              << std::setw(14) << "GFLOPs/s" << "\n";
    std::cout << std::string(52, '-') << "\n";

    auto bench_kernel = [&](const std::string& name, auto fn) {
        double ops = measure([&]{ fn(a.data(), b.data(), dim); });
        double gf  = ops * flops_per_call / 1e9;
        std::cout << std::left  << std::setw(20) << name
                  << std::right << std::setw(16) << std::fixed << std::setprecision(2) << ops / 1e6
                  << std::setw(14) << gf << "\n";
    };

    bench_kernel("Scalar",   vecdb::simd::l2_sq_scalar);
    bench_kernel("AVX2",     vecdb::simd::l2_sq_avx2);
    bench_kernel("AVX-512",  vecdb::simd::l2_sq_avx512);
    bench_kernel("Dispatch", vecdb::simd::l2_sq);
}

// ── HNSW insert benchmark ─────────────────────────────────────────────────────

void bench_hnsw_insert(int dim, int N) {
    std::cout << "\n── HNSW insert throughput  (dim=" << dim << ", N=" << N << ") ──────────\n";

    vecdb::HNSWConfig cfg;
    cfg.dim             = dim;
    cfg.M               = 16;
    cfg.M_max0          = 32;
    cfg.ef_construction = 200;
    cfg.arena_bytes     = 256ULL * 1024 * 1024;

    auto dataset = rand_matrix(N, dim);

    vecdb::HNSWIndex idx(cfg);

    auto start = Clock::now();
    for (auto& v : dataset) idx.insert(v.data());
    double elapsed = Dur(Clock::now() - start).count();

    std::cout << "  Inserted " << N << " vectors in "
              << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "  Throughput: "
              << std::setprecision(0) << (N / elapsed) << " inserts/s\n";
}

// ── HNSW search benchmark ─────────────────────────────────────────────────────

void bench_hnsw_search(int dim, int N, int top_k) {
    std::cout << "\n── HNSW search throughput  (dim=" << dim
              << ", N=" << N << ", top_k=" << top_k << ") ───────\n";

    vecdb::HNSWConfig cfg;
    cfg.dim             = dim;
    cfg.M               = 16;
    cfg.M_max0          = 32;
    cfg.ef_construction = 200;
    cfg.ef_search       = 64;
    cfg.arena_bytes     = 256ULL * 1024 * 1024;

    auto dataset = rand_matrix(N, dim);
    auto queries = rand_matrix(200, dim, 9999);

    vecdb::HNSWIndex idx(cfg);
    for (auto& v : dataset) idx.insert(v.data());

    std::cout << "  Index built (" << N << " vectors). Running queries...\n";

    int qi = 0;
    double ops = measure([&]{
        idx.search(queries[qi % queries.size()].data(), top_k);
        ++qi;
    }, 3.0);

    std::cout << "  Throughput: "
              << std::fixed << std::setprecision(0) << ops << " queries/s\n";

    // Latency percentiles
    std::vector<double> latencies;
    latencies.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        auto t0 = Clock::now();
        idx.search(queries[i % queries.size()].data(), top_k);
        latencies.push_back(Dur(Clock::now() - t0).count() * 1e6);
    }
    std::sort(latencies.begin(), latencies.end());
    std::cout << "  Latency  P50=" << std::setprecision(1) << latencies[500]
              << "µs  P95=" << latencies[950]
              << "µs  P99=" << latencies[990] << "µs\n";
}

// ── Speedup summary ────────────────────────────────────────────────────────────

void bench_speedup(int dim) {
    auto a = rand_vec(dim, 1);
    auto b = rand_vec(dim, 2);

    auto time_kernel = [&](auto fn) {
        return 1.0 / measure([&]{ fn(a.data(), b.data(), dim); }, 1.5);
    };

    double t_scalar = time_kernel(vecdb::simd::l2_sq_scalar);
    double t_avx2   = time_kernel(vecdb::simd::l2_sq_avx2);
    double t_avx512 = time_kernel(vecdb::simd::l2_sq_avx512);

    std::cout << "\n── Speedup vs scalar  (dim=" << dim << ") ─────────────────────────\n";
    std::cout << "  AVX2    speedup: " << std::fixed << std::setprecision(1)
              << (t_scalar / t_avx2)   << "x\n";
    std::cout << "  AVX-512 speedup: "
              << (t_scalar / t_avx512) << "x\n";
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    vecdb::simd::init();

    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║    vecdb Phase 2 — SIMD Distance Kernel Benchmark    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n";
    std::cout << "Active ISA: " << vecdb::simd::active_isa_name() << "\n";

    // Distance kernel at multiple dims
    bench_distance(128);
    bench_distance(768);
    bench_distance(1536);

    // Speedup summary
    bench_speedup(768);

    // HNSW insert
    bench_hnsw_insert(128, 50'000);

    // HNSW search at different scales
    bench_hnsw_search(128, 50'000,  10);
    bench_hnsw_search(128, 50'000, 100);

    std::cout << "\n✓ Benchmark complete.\n";
    return 0;
}
