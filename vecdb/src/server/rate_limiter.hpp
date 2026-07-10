#pragma once

// ──────────────────────────────────────────────────────────────────────────────
//  RateLimiter — Phase 6
//
//  Per-client token bucket rate limiter.
//  Each client (identified by peer address or token) gets a bucket of `capacity`
//  tokens that refills at `refill_per_sec` tokens/second.
//
//  Algorithm (token bucket):
//    - Bucket starts full (capacity tokens)
//    - Each request consumes 1 token
//    - Tokens refill continuously at refill_per_sec
//    - If bucket is empty, request is rejected (RESOURCE_EXHAUSTED)
//
//  Thread safety: a single mutex guards the bucket map.
// ──────────────────────────────────────────────────────────────────────────────

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>

#include <grpcpp/grpcpp.h>

namespace vecdb {

class RateLimiter {
public:
    // `capacity`       : max burst size (tokens)
    // `refill_per_sec` : steady-state rate (tokens/second)
    RateLimiter(double capacity = 100.0, double refill_per_sec = 50.0);

    // Attempt to consume one token for `client_id`.
    // Returns OK if allowed, RESOURCE_EXHAUSTED if rate limit hit.
    grpc::Status check(const std::string& client_id);

    // For testing: disable rate limiting entirely
    void set_disabled(bool disabled) { disabled_ = disabled; }

private:
    struct Bucket {
        double   tokens;
        std::chrono::steady_clock::time_point last_refill;
    };

    double   capacity_;
    double   refill_per_sec_;
    bool     disabled_ = false;

    std::mutex                             mu_;
    std::unordered_map<std::string, Bucket> buckets_;
};

} // namespace vecdb
