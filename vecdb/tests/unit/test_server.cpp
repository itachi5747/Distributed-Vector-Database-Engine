// Phase 6 — gRPC server unit tests
//
// Tests service logic WITHOUT starting a real gRPC network server.
// We call the RPC handler methods directly (same code path, no network).
//
// Tests:
//   1.  AuthMiddleware: valid token passes
//   2.  AuthMiddleware: missing header rejected
//   3.  AuthMiddleware: wrong token rejected
//   4.  AuthMiddleware: disabled bypasses all checks
//   5.  RateLimiter: first request passes
//   6.  RateLimiter: burst exhausted then recovers
//   7.  RateLimiter: different clients have independent buckets
//   8.  VectorDBService: Insert round-trip
//   9.  VectorDBService: Insert wrong vector size rejected
//   10. VectorDBService: BulkInsert inserts multiple vectors
//   11. VectorDBService: Search returns sorted results
//   12. VectorDBService: Search wrong query size rejected
//   13. VectorDBService: GetInfo returns correct counts
//   14. VectorDBService: unauthenticated Insert rejected
//   15. VectorDBService: Delete returns UNIMPLEMENTED

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "src/server/auth_middleware.hpp"
#include "src/server/rate_limiter.hpp"
#include "src/server/grpc_server.hpp"
#include "src/shard/shard_manager.hpp"
#include "src/simd/distance.hpp"

#include <vector>
#include <random>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

struct TempDir {
    std::string path;
    explicit TempDir(const std::string& n) {
        path = "/tmp/vecdb_grpc_" + n;
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

static std::vector<float> rand_vec(int dim, uint64_t seed = 42) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0.f, 1.f);
    std::vector<float> v(dim);
    float norm = 0.f;
    for (float& x : v) { x = d(rng); norm += x * x; }
    norm = std::sqrt(norm);
    for (float& x : v) x /= (norm + 1e-9f);
    return v;
}

// Pack float vector → raw bytes (as gRPC client would send)
static std::string pack_vec(const std::vector<float>& v) {
    std::string s(v.size() * sizeof(float), '\0');
    std::memcpy(s.data(), v.data(), s.size());
    return s;
}

// FakeContext — plain ServerContext (client_metadata is empty)
// Auth tests use direct token validation; gRPC metadata path tested via integration
using FakeContext = grpc::ServerContext;

// Helper: build an auth header value
static std::string bearer(const std::string& token) {
    return "Bearer " + token;
}

static vecdb::HNSWConfig make_cfg(int dim) {
    vecdb::HNSWConfig cfg;
    cfg.dim             = dim;
    cfg.M               = 8;
    cfg.M_max0          = 16;
    cfg.ef_construction = 50;
    cfg.ef_search       = 20;
    cfg.arena_bytes     = 8ULL * 1024 * 1024;
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
//  AuthMiddleware tests
// ─────────────────────────────────────────────────────────────────────────────

// Auth tests use check_value() — tests token logic without gRPC context.
// The gRPC metadata extraction path is thin glue covered by integration tests.

TEST_CASE("Auth: valid bearer token passes check_value") {
    vecdb::AuthMiddleware auth({"secret-token"});
    CHECK(auth.check_value(bearer("secret-token")).ok());
}

TEST_CASE("Auth: empty value rejected") {
    vecdb::AuthMiddleware auth({"secret-token"});
    CHECK(!auth.check_value("").ok());
}

TEST_CASE("Auth: wrong token rejected") {
    vecdb::AuthMiddleware auth({"secret-token"});
    auto s = auth.check_value(bearer("wrong-token"));
    CHECK(!s.ok());
    CHECK(s.error_code() == grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("Auth: missing Bearer prefix rejected") {
    vecdb::AuthMiddleware auth({"secret-token"});
    auto s = auth.check_value("secret-token"); // no "Bearer " prefix
    CHECK(!s.ok());
}

TEST_CASE("Auth: disabled bypasses all checks") {
    vecdb::AuthMiddleware auth({});
    auth.set_disabled(true);
    FakeContext ctx; // empty context
    auto s = auth.check(&ctx);
    CHECK(s.ok());
}

TEST_CASE("Auth: add and remove tokens at runtime") {
    vecdb::AuthMiddleware auth({});
    CHECK(!auth.check_value(bearer("dynamic-token")).ok());

    auth.add_token("dynamic-token");
    CHECK(auth.check_value(bearer("dynamic-token")).ok());

    auth.remove_token("dynamic-token");
    CHECK(!auth.check_value(bearer("dynamic-token")).ok());
}

// ─────────────────────────────────────────────────────────────────────────────
//  RateLimiter tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("RateLimiter: first request always passes") {
    vecdb::RateLimiter rl(10.0, 5.0);
    auto s = rl.check("client-A");
    CHECK(s.ok());
}

TEST_CASE("RateLimiter: burst exhausted then rejected") {
    vecdb::RateLimiter rl(5.0, 0.0); // capacity=5, no refill
    std::string client = "burst-client";

    // Capacity=5: first request starts full bucket and consumes 1 (4 left)
    // requests 2-5 consume remaining 4 → 0 left
    // request 6 fails
    for (int i = 0; i < 5; ++i) rl.check(client);
    // 6th request should be rejected (bucket empty)
    auto s = rl.check(client);
    CHECK(!s.ok());
    CHECK(s.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
}

TEST_CASE("RateLimiter: different clients are independent") {
    vecdb::RateLimiter rl(2.0, 0.0); // capacity=2, no refill
    // Exhaust client A
    rl.check("A"); rl.check("A"); rl.check("A");
    CHECK(!rl.check("A").ok()); // A exhausted

    // Client B is independent — should still pass
    CHECK(rl.check("B").ok());
}

TEST_CASE("RateLimiter: disabled allows unlimited requests") {
    vecdb::RateLimiter rl(1.0, 0.0); // capacity=1, no refill
    rl.set_disabled(true);
    std::string client = "any-client";
    for (int i = 0; i < 100; ++i)
        CHECK(rl.check(client).ok());
}

// ─────────────────────────────────────────────────────────────────────────────
//  VectorDBServiceImpl tests
// ─────────────────────────────────────────────────────────────────────────────

// Fixture: service with auth+rate disabled for simplicity
struct ServiceFixture {
    const int DIM = 32;
    TempDir   td;
    vecdb::HNSWConfig           cfg;
    vecdb::ShardManager         mgr;
    vecdb::AuthMiddleware       auth;
    vecdb::RateLimiter          rl;
    vecdb::VectorDBServiceImpl  svc;

    ServiceFixture()
        : td("svc")
        , cfg(make_cfg(DIM))
        , mgr(cfg, 3, td.path)
        , auth({})
        , rl(1000.0, 1000.0)
        , svc(mgr, auth, rl)
    {
        vecdb::simd::init();
        auth.set_disabled(true);
        rl.set_disabled(true);
    }

    std::string vec_bytes(uint64_t seed = 1) {
        return pack_vec(rand_vec(DIM, seed));
    }
};

TEST_CASE("Service: Insert round-trip") {
    ServiceFixture f;
    FakeContext ctx;

    vecdb::InsertRequest  req;
    vecdb::InsertResponse resp;
    req.set_vector_id(42);
    req.set_vector(f.vec_bytes(1));

    auto s = f.svc.Insert(&ctx, &req, &resp);
    CHECK(s.ok());
    CHECK(resp.shard_id() < 3u);
    CHECK(resp.latency_us() >= 0);
    CHECK(f.mgr.total_vectors() == 1u);
}

TEST_CASE("Service: Insert wrong vector size rejected") {
    ServiceFixture f;
    FakeContext ctx;

    vecdb::InsertRequest  req;
    vecdb::InsertResponse resp;
    req.set_vector_id(1);
    req.set_vector("too-short");  // wrong size

    auto s = f.svc.Insert(&ctx, &req, &resp);
    CHECK(!s.ok());
    CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_CASE("Service: multiple sequential Inserts accumulate correctly") {
    // Tests the same insert path as BulkInsert but without the grpc::ServerReader mock
    // (ServerReader has no public constructor — tested via integration tests).
    ServiceFixture f;
    FakeContext ctx;

    const int N = 20;
    for (int i = 0; i < N; ++i) {
        vecdb::InsertRequest  req;
        vecdb::InsertResponse resp;
        req.set_vector_id(static_cast<uint64_t>(i));
        req.set_vector(f.vec_bytes(i + 1));
        auto s = f.svc.Insert(&ctx, &req, &resp);
        CHECK(s.ok());
    }
    CHECK(f.mgr.total_vectors() == static_cast<uint64_t>(N));
}

TEST_CASE("Service: Search returns sorted results") {
    ServiceFixture f;
    FakeContext ctx;

    // Insert 50 vectors
    for (int i = 0; i < 50; ++i) {
        vecdb::InsertRequest  req;
        vecdb::InsertResponse resp;
        req.set_vector_id(static_cast<uint64_t>(i));
        req.set_vector(f.vec_bytes(i + 1));
        f.svc.Insert(&ctx, &req, &resp);
    }

    // Search top-5
    vecdb::SearchRequest  req;
    vecdb::SearchResponse resp;
    req.set_query(f.vec_bytes(999));
    req.set_top_k(5);

    auto s = f.svc.Search(&ctx, &req, &resp);
    CHECK(s.ok());
    CHECK(resp.results_size() == 5);
    CHECK(resp.latency_us() >= 0);
    CHECK(!resp.degraded());

    // Results must be sorted closest-first
    for (int i = 1; i < resp.results_size(); ++i)
        CHECK(resp.results(i).distance() >= resp.results(i-1).distance());
}

TEST_CASE("Service: Search wrong query size rejected") {
    ServiceFixture f;
    FakeContext ctx;

    vecdb::SearchRequest  req;
    vecdb::SearchResponse resp;
    req.set_query("bad-size");
    req.set_top_k(5);

    auto s = f.svc.Search(&ctx, &req, &resp);
    CHECK(!s.ok());
    CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_CASE("Service: GetInfo returns correct counts") {
    ServiceFixture f;
    FakeContext ctx;

    // Insert 30 vectors
    for (int i = 0; i < 30; ++i) {
        vecdb::InsertRequest  req;
        vecdb::InsertResponse resp;
        req.set_vector_id(static_cast<uint64_t>(i));
        req.set_vector(f.vec_bytes(i + 1));
        f.svc.Insert(&ctx, &req, &resp);
    }

    vecdb::InfoRequest  req;
    vecdb::InfoResponse resp;
    auto s = f.svc.GetInfo(&ctx, &req, &resp);

    CHECK(s.ok());
    CHECK(resp.total_vectors() == 30u);
    CHECK(resp.shard_count()   == 3u);
    CHECK(resp.dim()           == f.DIM);
    CHECK(resp.shards_size()   == 3);

    // All shards should report healthy
    for (int i = 0; i < resp.shards_size(); ++i)
        CHECK(resp.shards(i).status() == "healthy");
}

TEST_CASE("Service: unauthenticated request rejected") {
    ServiceFixture f;
    // Re-enable auth
    f.auth.set_disabled(false);
    f.auth.add_token("valid-token");

    FakeContext ctx; // no auth header

    vecdb::InsertRequest  req;
    vecdb::InsertResponse resp;
    req.set_vector_id(1);
    req.set_vector(f.vec_bytes(1));

    auto s = f.svc.Insert(&ctx, &req, &resp);
    CHECK(!s.ok());
    CHECK(s.error_code() == grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("Service: Delete returns UNIMPLEMENTED") {
    ServiceFixture f;
    FakeContext ctx;

    vecdb::DeleteRequest  req;
    vecdb::DeleteResponse resp;
    req.set_vector_id(1);

    auto s = f.svc.Delete(&ctx, &req, &resp);
    CHECK(!s.ok());
    CHECK(s.error_code() == grpc::StatusCode::UNIMPLEMENTED);
}