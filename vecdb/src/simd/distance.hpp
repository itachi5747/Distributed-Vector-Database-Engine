#pragma once

#include <cstdint>

// ──────────────────────────────────────────────────────────────────────────────
//  SIMD Distance Kernels — Phase 2
//
//  Three tiers, selected at runtime via CPUID:
//    AVX-512  →  16 floats/cycle  (best, Intel Skylake-X / Ice Lake+)
//    AVX2     →   8 floats/cycle  (most modern x86-64 CPUs)
//    Scalar   →   1 float/cycle   (fallback, always correct)
//
//  Public API: call via the dispatch pointer — never call _avx2/_avx512 directly.
//
//  Usage:
//    vecdb::simd::init();                         // call once at startup
//    float d = vecdb::simd::l2_sq(a, b, dim);    // always picks fastest kernel
// ──────────────────────────────────────────────────────────────────────────────

namespace vecdb::simd {

// ── Raw kernels (exposed for unit testing / benchmarking) ─────────────────────
float l2_sq_scalar  (const float* a, const float* b, int dim);
float l2_sq_avx2    (const float* a, const float* b, int dim);
float l2_sq_avx512  (const float* a, const float* b, int dim);

float cosine_scalar (const float* a, const float* b, int dim);
float cosine_avx2   (const float* a, const float* b, int dim);
float cosine_avx512 (const float* a, const float* b, int dim);

// ── Runtime dispatch ──────────────────────────────────────────────────────────

// Which ISA level was selected at startup
enum class ISALevel : uint8_t { Scalar, AVX2, AVX512 };

// Call once before any distance computation (reads CPUID, sets function pointers)
void init();

// Current ISA level (for logging/testing)
ISALevel active_isa();
const char* active_isa_name();

// Dispatch function pointers — set by init(), called on the hot path
// These are plain function pointers (not std::function) for zero-overhead calls
extern float (*l2_sq)  (const float* a, const float* b, int dim);
extern float (*cosine) (const float* a, const float* b, int dim);

} // namespace vecdb::simd
