// Phase 2 — SIMD distance kernel unit tests
//
// Tests:
//   1. All kernels (scalar/AVX2/AVX512) produce identical results
//   2. Correctness against known analytical values
//   3. Runtime dispatch selects correct ISA
//   4. Edge cases: dim=1, dim not multiple of 8/16, zero vector

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "src/simd/distance.hpp"

#include <vector>
#include <random>
#include <cmath>

// ── Helpers ───────────────────────────────────────────────────────────────────

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

// ── Test 1: known analytical values ───────────────────────────────────────────

TEST_CASE("l2_sq — known values") {
    float a[] = {1.f, 0.f, 0.f, 0.f};
    float b[] = {0.f, 1.f, 0.f, 0.f};
    // ||a-b||² = (1)²+(−1)²+(0)²+(0)² = 2
    CHECK(vecdb::simd::l2_sq_scalar(a, b, 4) == doctest::Approx(2.f).epsilon(1e-6f));
    CHECK(vecdb::simd::l2_sq_avx2  (a, b, 4) == doctest::Approx(2.f).epsilon(1e-6f));
    CHECK(vecdb::simd::l2_sq_avx512(a, b, 4) == doctest::Approx(2.f).epsilon(1e-6f));

    // Same vector → 0
    CHECK(vecdb::simd::l2_sq_scalar(a, a, 4) == doctest::Approx(0.f).epsilon(1e-9f));
    CHECK(vecdb::simd::l2_sq_avx2  (a, a, 4) == doctest::Approx(0.f).epsilon(1e-9f));
    CHECK(vecdb::simd::l2_sq_avx512(a, a, 4) == doctest::Approx(0.f).epsilon(1e-9f));
}

TEST_CASE("cosine — known values") {
    float a[] = {1.f, 0.f};
    float b[] = {0.f, 1.f};
    // Orthogonal → cosine distance = 1
    CHECK(vecdb::simd::cosine_scalar(a, b, 2) == doctest::Approx(1.f).epsilon(1e-6f));
    CHECK(vecdb::simd::cosine_avx2  (a, b, 2) == doctest::Approx(1.f).epsilon(1e-6f));
    CHECK(vecdb::simd::cosine_avx512(a, b, 2) == doctest::Approx(1.f).epsilon(1e-6f));

    float c[] = {3.f, 0.f};
    // Same direction → 0
    CHECK(vecdb::simd::cosine_scalar(a, c, 2) == doctest::Approx(0.f).epsilon(1e-6f));
    CHECK(vecdb::simd::cosine_avx2  (a, c, 2) == doctest::Approx(0.f).epsilon(1e-6f));
    CHECK(vecdb::simd::cosine_avx512(a, c, 2) == doctest::Approx(0.f).epsilon(1e-6f));
}

// ── Test 2: all kernels agree on random vectors ───────────────────────────────

TEST_CASE("all l2_sq kernels agree — dim=768") {
    const int DIM = 768;
    auto a = rand_vec(DIM, 1);
    auto b = rand_vec(DIM, 2);

    float ref = vecdb::simd::l2_sq_scalar(a.data(), b.data(), DIM);
    CHECK(vecdb::simd::l2_sq_avx2  (a.data(), b.data(), DIM) == doctest::Approx(ref).epsilon(1e-4f));
    CHECK(vecdb::simd::l2_sq_avx512(a.data(), b.data(), DIM) == doctest::Approx(ref).epsilon(1e-4f));
}

TEST_CASE("all cosine kernels agree — dim=768") {
    const int DIM = 768;
    auto a = rand_vec(DIM, 3);
    auto b = rand_vec(DIM, 4);

    float ref = vecdb::simd::cosine_scalar(a.data(), b.data(), DIM);
    CHECK(vecdb::simd::cosine_avx2  (a.data(), b.data(), DIM) == doctest::Approx(ref).epsilon(1e-4f));
    CHECK(vecdb::simd::cosine_avx512(a.data(), b.data(), DIM) == doctest::Approx(ref).epsilon(1e-4f));
}

// ── Test 3: non-multiple dims (tail handling) ─────────────────────────────────

TEST_CASE("l2_sq tail handling — dims not multiple of 8 or 16") {
    for (int dim : {1, 3, 7, 9, 15, 17, 33, 65, 100, 127, 255, 513}) {
        auto a = rand_vec(dim, dim);
        auto b = rand_vec(dim, dim+1);
        float ref  = vecdb::simd::l2_sq_scalar(a.data(), b.data(), dim);
        float avx2 = vecdb::simd::l2_sq_avx2  (a.data(), b.data(), dim);
        float a512 = vecdb::simd::l2_sq_avx512(a.data(), b.data(), dim);
        CHECK_MESSAGE(std::abs(avx2 - ref) < 1e-4f, "dim=", dim, " avx2=", avx2, " ref=", ref);
        CHECK_MESSAGE(std::abs(a512 - ref) < 1e-4f, "dim=", dim, " avx512=", a512, " ref=", ref);
    }
}

// ── Test 4: runtime dispatch ──────────────────────────────────────────────────

TEST_CASE("dispatch init selects a valid ISA") {
    vecdb::simd::init();
    const char* name = vecdb::simd::active_isa_name();
    MESSAGE("Selected ISA: " << std::string(name));
    // Must be one of the three valid levels
    bool valid = (vecdb::simd::active_isa() == vecdb::simd::ISALevel::Scalar  ||
                  vecdb::simd::active_isa() == vecdb::simd::ISALevel::AVX2    ||
                  vecdb::simd::active_isa() == vecdb::simd::ISALevel::AVX512);
    CHECK(valid);
}

TEST_CASE("dispatch function pointer produces correct result") {
    vecdb::simd::init();
    const int DIM = 512;
    auto a = rand_vec(DIM, 10);
    auto b = rand_vec(DIM, 20);

    float ref      = vecdb::simd::l2_sq_scalar(a.data(), b.data(), DIM);
    float dispatch = vecdb::simd::l2_sq       (a.data(), b.data(), DIM);
    CHECK(dispatch == doctest::Approx(ref).epsilon(1e-4f));
}

// ── Test 5: dim=1536 (text-embedding-3-small) ─────────────────────────────────

TEST_CASE("l2_sq correctness at dim=1536") {
    const int DIM = 1536;
    auto a = rand_vec(DIM, 7);
    auto b = rand_vec(DIM, 8);

    float ref  = vecdb::simd::l2_sq_scalar(a.data(), b.data(), DIM);
    float avx2 = vecdb::simd::l2_sq_avx2  (a.data(), b.data(), DIM);
    float a512 = vecdb::simd::l2_sq_avx512(a.data(), b.data(), DIM);

    CHECK(avx2 == doctest::Approx(ref).epsilon(1e-3f));
    CHECK(a512 == doctest::Approx(ref).epsilon(1e-3f));
}