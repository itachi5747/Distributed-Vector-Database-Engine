#include "rate_limiter.hpp"

namespace vecdb {

RateLimiter::RateLimiter(double capacity, double refill_per_sec)
    : capacity_(capacity)
    , refill_per_sec_(refill_per_sec)
{}

grpc::Status RateLimiter::check(const std::string& client_id) {
    if (disabled_) return grpc::Status::OK;

    std::lock_guard<std::mutex> lock(mu_);
    auto now = std::chrono::steady_clock::now();

    auto it = buckets_.find(client_id);
    if (it == buckets_.end()) {
        // New client — start with full bucket, consume 1 token for this request
        buckets_[client_id] = { capacity_ - 1.0, now };
        return grpc::Status::OK;
    }

    Bucket& b = it->second;

    // Refill based on elapsed time
    double elapsed = std::chrono::duration<double>(now - b.last_refill).count();
    b.tokens = std::min(capacity_, b.tokens + elapsed * refill_per_sec_);
    b.last_refill = now;

    // Consume 1 token
    b.tokens -= 1.0;

    if (b.tokens < 0.0) {
        b.tokens = 0.0; // clamp so we don't go deeply negative
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "Rate limit exceeded — slow down requests");
    }

    return grpc::Status::OK;
}

} // namespace vecdb
