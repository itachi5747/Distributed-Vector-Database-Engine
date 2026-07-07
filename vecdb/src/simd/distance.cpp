// ──────────────────────────────────────────────────────────────────────────────
//  SIMD Distance Kernels — Phase 2
//
//  Runtime dispatch flow:
//    1. init() reads CPUID + XGETBV (OS XSAVE check)
//    2. Sets l2_sq / cosine function pointers to best available kernel
//    3. HNSWIndex constructor calls init() automatically (idempotent)
//
//  Kernel ISA requirements:
//    AVX-512 : AVX512F + AVX512DQ + FMA + OS XSAVE for ZMM state
//    AVX2    : AVX2 + FMA + OS XSAVE for YMM state
//    Scalar  : always available
// ──────────────────────────────────────────────────────────────────────────────

#include "distance.hpp"

#include <immintrin.h>
#include <cpuid.h>
#include <cmath>
#include <cstdint>
#include <atomic>

namespace vecdb::simd {

#if defined(__GNUC__)
#define VECDB_NO_TREE_VECTORIZE __attribute__((optimize("no-tree-vectorize")))
#else
#define VECDB_NO_TREE_VECTORIZE
#endif

// ── Dispatch pointers (default = scalar until init() called) ──────────────────
float (*l2_sq)  (const float*, const float*, int) = l2_sq_scalar;
float (*cosine) (const float*, const float*, int) = cosine_scalar;

// ── Scalar kernels ────────────────────────────────────────────────────────────

VECDB_NO_TREE_VECTORIZE
float l2_sq_scalar(const float* __restrict__ a,
                   const float* __restrict__ b,
                   int dim) {
    float acc = 0.0f;
    for (int i = 0; i < dim; ++i) {
        float d = a[i] - b[i];
        acc += d * d;
    }
    return acc;
}

VECDB_NO_TREE_VECTORIZE
float cosine_scalar(const float* __restrict__ a,
                    const float* __restrict__ b,
                    int dim) {
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (int i = 0; i < dim; ++i) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    if (denom < 1e-10f) return 1.0f;
    return 1.0f - (dot / denom);
}

// ── AVX2 kernels ──────────────────────────────────────────────────────────────

static bool supports_avx2();
static bool supports_avx512();

__attribute__((target("avx2,fma")))
float l2_sq_avx2(const float* __restrict__ a,
                 const float* __restrict__ b,
                 int dim) {
    __m256 acc = _mm256_setzero_ps();
    int i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va   = _mm256_loadu_ps(a + i);
        __m256 vb   = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        acc = _mm256_fmadd_ps(diff, diff, acc);
    }
    // Horizontal reduction
    __m128 lo  = _mm256_castps256_ps128(acc);
    __m128 hi  = _mm256_extractf128_ps(acc, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    float result = _mm_cvtss_f32(sum);
    // Scalar tail
    for (; i < dim; ++i) { float d = a[i]-b[i]; result += d*d; }
    return result;
}

__attribute__((target("avx2,fma")))
float cosine_avx2(const float* __restrict__ a,
                  const float* __restrict__ b,
                  int dim) {
    __m256 vdot = _mm256_setzero_ps();
    __m256 vna  = _mm256_setzero_ps();
    __m256 vnb  = _mm256_setzero_ps();
    int i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        vdot = _mm256_fmadd_ps(va, vb, vdot);
        vna  = _mm256_fmadd_ps(va, va, vna);
        vnb  = _mm256_fmadd_ps(vb, vb, vnb);
    }
    auto hsum = [](__m256 v) -> float {
        __m128 lo = _mm256_castps256_ps128(v);
        __m128 hi = _mm256_extractf128_ps(v, 1);
        __m128 s  = _mm_add_ps(lo, hi);
        s = _mm_hadd_ps(s, s);
        s = _mm_hadd_ps(s, s);
        return _mm_cvtss_f32(s);
    };
    float dot = hsum(vdot), na = hsum(vna), nb = hsum(vnb);
    for (; i < dim; ++i) { dot+=a[i]*b[i]; na+=a[i]*a[i]; nb+=b[i]*b[i]; }
    float denom = std::sqrt(na) * std::sqrt(nb);
    if (denom < 1e-10f) return 1.0f;
    return 1.0f - (dot / denom);
}

// ── AVX-512 kernels ───────────────────────────────────────────────────────────

__attribute__((target("avx512f,avx512dq,fma")))
float l2_sq_avx512(const float* __restrict__ a,
                   const float* __restrict__ b,
                   int dim) {
    if (!supports_avx512()) {
        return supports_avx2() ? l2_sq_avx2(a, b, dim) : l2_sq_scalar(a, b, dim);
    }
    __m512 acc = _mm512_setzero_ps();
    int i = 0;
    for (; i + 16 <= dim; i += 16) {
        __m512 va   = _mm512_loadu_ps(a + i);
        __m512 vb   = _mm512_loadu_ps(b + i);
        __m512 diff = _mm512_sub_ps(va, vb);
        acc = _mm512_fmadd_ps(diff, diff, acc);
    }
    float result = _mm512_reduce_add_ps(acc);
    for (; i < dim; ++i) { float d = a[i]-b[i]; result += d*d; }
    return result;
}

__attribute__((target("avx512f,avx512dq,fma")))
float cosine_avx512(const float* __restrict__ a,
                    const float* __restrict__ b,
                    int dim) {
    if (!supports_avx512()) {
        return supports_avx2() ? cosine_avx2(a, b, dim) : cosine_scalar(a, b, dim);
    }
    __m512 vdot = _mm512_setzero_ps();
    __m512 vna  = _mm512_setzero_ps();
    __m512 vnb  = _mm512_setzero_ps();
    int i = 0;
    for (; i + 16 <= dim; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        vdot = _mm512_fmadd_ps(va, vb, vdot);
        vna  = _mm512_fmadd_ps(va, va, vna);
        vnb  = _mm512_fmadd_ps(vb, vb, vnb);
    }
    float dot = _mm512_reduce_add_ps(vdot);
    float na  = _mm512_reduce_add_ps(vna);
    float nb  = _mm512_reduce_add_ps(vnb);
    for (; i < dim; ++i) { dot+=a[i]*b[i]; na+=a[i]*a[i]; nb+=b[i]*b[i]; }
    float denom = std::sqrt(na) * std::sqrt(nb);
    if (denom < 1e-10f) return 1.0f;
    return 1.0f - (dot / denom);
}

// ── CPUID helpers ─────────────────────────────────────────────────────────────

static bool os_saves_ymm() {
    // Check CPUID leaf 1 ECX bit 27 (OSXSAVE), then XGETBV XCR0 bits 1+2 (YMM)
    uint32_t eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    if (!((ecx >> 27) & 1)) return false;       // OSXSAVE not set
    uint64_t xcr0 = _xgetbv(0);
    return (xcr0 & 0x6) == 0x6;                 // XMM + YMM state saved
}

static bool os_saves_zmm() {
    if (!os_saves_ymm()) return false;
    uint64_t xcr0 = _xgetbv(0);
    return (xcr0 & 0xE6) == 0xE6;               // YMM + ZMM upper + ZMM16-31
}

struct CPUFeatures {
    bool avx2   = false;
    bool avx512 = false;
    bool fma    = false;
};

static CPUFeatures detect_cpu() {
    CPUFeatures f;
    uint32_t eax, ebx, ecx, edx;

    // FMA: CPUID leaf 1 ECX bit 12
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        f.fma = (ecx >> 12) & 1;

    // AVX2: leaf 7 EBX bit 5 | AVX-512F: leaf 7 EBX bit 16
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        f.avx2   = (ebx >> 5)  & 1;
        f.avx512 = (ebx >> 16) & 1;
    }

    // Also require the OS to enable the relevant XSAVE state
    if (f.avx512) f.avx512 = os_saves_zmm();
    if (f.avx2)   f.avx2   = os_saves_ymm();

    return f;
}

static const CPUFeatures& cached_cpu_features() {
    static const CPUFeatures feat = detect_cpu();
    return feat;
}

static bool supports_avx2() {
    auto feat = cached_cpu_features();
    return feat.avx2 && feat.fma;
}

static bool supports_avx512() {
    auto feat = cached_cpu_features();
    return feat.avx512 && feat.fma;
}

// ── Runtime dispatch init (idempotent) ────────────────────────────────────────

static ISALevel        g_isa         = ISALevel::Scalar;
static std::atomic_bool g_initialised{false};

void init() {
    // Fast path — already done
    if (g_initialised.load(std::memory_order_acquire)) return;

    auto feat = cached_cpu_features();

    if (feat.avx512 && feat.fma) {
        l2_sq  = l2_sq_avx512;
        cosine = cosine_avx512;
        g_isa  = ISALevel::AVX512;
    } else if (feat.avx2 && feat.fma) {
        l2_sq  = l2_sq_avx2;
        cosine = cosine_avx2;
        g_isa  = ISALevel::AVX2;
    } else {
        l2_sq  = l2_sq_scalar;
        cosine = cosine_scalar;
        g_isa  = ISALevel::Scalar;
    }

    g_initialised.store(true, std::memory_order_release);
}

ISALevel    active_isa()      { return g_isa; }
const char* active_isa_name() {
    switch (g_isa) {
        case ISALevel::AVX512: return "AVX-512";
        case ISALevel::AVX2:   return "AVX2";
        default:               return "Scalar";
    }
}

} // namespace vecdb::simd
