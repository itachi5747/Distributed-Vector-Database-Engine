#include "grpc_server.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <cmath>
#include <sstream>

namespace vecdb {

using Clock = std::chrono::high_resolution_clock;
using Dur   = std::chrono::duration<double, std::micro>;

// ── Constructor ───────────────────────────────────────────────────────────────

VectorDBServiceImpl::VectorDBServiceImpl(ShardManager&   shard_mgr,
                                          AuthMiddleware& auth,
                                          RateLimiter&    rate_limiter)
    : shard_mgr_(shard_mgr)
    , auth_(auth)
    , rate_limiter_(rate_limiter)
    , dim_(shard_mgr.cfg_dim())
{}

// ── gate — auth + rate limit ──────────────────────────────────────────────────

grpc::Status VectorDBServiceImpl::gate(grpc::ServerContext* ctx) {
    grpc::Status s = auth_.check(ctx);
    if (!s.ok()) return s;

    std::string peer = ctx->peer(); // "ipv4:127.0.0.1:PORT" or similar
    return rate_limiter_.check(peer);
}

// ── decode_vector ─────────────────────────────────────────────────────────────

bool VectorDBServiceImpl::decode_vector(const std::string& bytes,
                                         std::vector<float>& out) const {
    if (bytes.size() != static_cast<size_t>(dim_) * sizeof(float)) return false;
    out.resize(dim_);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

// ── Insert ────────────────────────────────────────────────────────────────────

grpc::Status VectorDBServiceImpl::Insert(grpc::ServerContext*        ctx,
                                          const vecdb::InsertRequest* req,
                                          vecdb::InsertResponse*      resp) {
    grpc::Status gs = gate(ctx);
    if (!gs.ok()) return gs;

    std::vector<float> vec;
    if (!decode_vector(req->vector(), vec)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
            "vector bytes size must be dim * 4 (float32)");
    }

    auto t0 = Clock::now();
    auto [shard_id, node_id] = shard_mgr_.insert(req->vector_id(), vec.data());
    int64_t us = static_cast<int64_t>(Dur(Clock::now() - t0).count());

    resp->set_shard_id(shard_id);
    resp->set_node_id(node_id);
    resp->set_latency_us(us);
    return grpc::Status::OK;
}

// ── BulkInsert ────────────────────────────────────────────────────────────────

grpc::Status VectorDBServiceImpl::BulkInsert(
    grpc::ServerContext*                      ctx,
    grpc::ServerReader<vecdb::InsertRequest>* reader,
    vecdb::BulkInsertResponse*                resp)
{
    grpc::Status gs = gate(ctx);
    if (!gs.ok()) return gs;

    auto t0 = Clock::now();
    uint64_t inserted = 0, failed = 0;

    vecdb::InsertRequest req;
    while (reader->Read(&req)) {
        std::vector<float> vec;
        if (!decode_vector(req.vector(), vec)) { ++failed; continue; }

        try {
            shard_mgr_.insert(req.vector_id(), vec.data());
            ++inserted;
        } catch (...) {
            ++failed;
        }
    }

    int64_t us = static_cast<int64_t>(Dur(Clock::now() - t0).count());
    resp->set_inserted(inserted);
    resp->set_failed(failed);
    resp->set_latency_us(us);
    return grpc::Status::OK;
}

// ── Search ────────────────────────────────────────────────────────────────────

grpc::Status VectorDBServiceImpl::Search(grpc::ServerContext*        ctx,
                                          const vecdb::SearchRequest* req,
                                          vecdb::SearchResponse*      resp) {
    grpc::Status gs = gate(ctx);
    if (!gs.ok()) return gs;

    if (req->top_k() <= 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "top_k must be > 0");
    }

    std::vector<float> query;
    if (!decode_vector(req->query(), query)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
            "query bytes size must be dim * 4 (float32)");
    }

    auto t0 = Clock::now();
    ShardSearchResult sr = shard_mgr_.search(
        query.data(), req->top_k(), req->ef_search());
    int64_t us = static_cast<int64_t>(Dur(Clock::now() - t0).count());

    for (const auto& hnsw_r : sr.results) {
        auto* res = resp->add_results();
        res->set_shard_id(0);
        // sr.results contains HNSWIndex HNSWSearchResult structs (node_id + dist fields)
        res->set_node_id(hnsw_r.node_id);
        res->set_distance(std::sqrt(hnsw_r.dist));
    }

    resp->set_latency_us(us);
    resp->set_degraded(sr.degraded);
    resp->set_shards_ok(sr.shards_ok);
    resp->set_shards_err(sr.shards_err);
    return grpc::Status::OK;
}

// ── Delete ────────────────────────────────────────────────────────────────────
// Phase 5 ShardManager doesn't implement delete (HNSW soft-delete is Phase 7+).
// Return UNIMPLEMENTED with a clear message.

grpc::Status VectorDBServiceImpl::Delete(grpc::ServerContext*        ctx,
                                          const vecdb::DeleteRequest* req,
                                          vecdb::DeleteResponse*      resp) {
    grpc::Status gs = gate(ctx);
    if (!gs.ok()) return gs;

    (void)req;
    resp->set_found(false);
    resp->set_latency_us(0);
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "Delete is not yet implemented");
}

// ── GetInfo ───────────────────────────────────────────────────────────────────

grpc::Status VectorDBServiceImpl::GetInfo(grpc::ServerContext*     ctx,
                                           const vecdb::InfoRequest* req,
                                           vecdb::InfoResponse*      resp) {
    grpc::Status gs = gate(ctx);
    if (!gs.ok()) return gs;

    (void)req;
    resp->set_total_vectors(shard_mgr_.total_vectors());
    resp->set_shard_count(shard_mgr_.shard_count());
    resp->set_dim(dim_);

    auto dist = shard_mgr_.load_distribution();
    for (uint32_t s = 0; s < shard_mgr_.shard_count(); ++s) {
        auto* si = resp->add_shards();
        si->set_shard_id(s);
        si->set_vector_count(dist[s]);
        si->set_status(shard_mgr_.shard_healthy(s) ? "healthy" : "degraded");
    }
    return grpc::Status::OK;
}

// ── run_server ────────────────────────────────────────────────────────────────

void run_server(ShardManager&       shard_mgr,
                AuthMiddleware&     auth,
                RateLimiter&        rate_limiter,
                const ServerConfig& cfg) {
    VectorDBServiceImpl service(shard_mgr, auth, rate_limiter);

    grpc::ServerBuilder builder;

    // Using InsecureServerCredentials (grpc++_unsecure; TLS can be added with full gRPC+OpenSSL)
    builder.AddListeningPort(cfg.listen_addr, grpc::InsecureServerCredentials());

    builder.RegisterService(&service);

    // One completion-queue thread per CPU core
    builder.SetSyncServerOption(
        grpc::ServerBuilder::SyncServerOption::NUM_CQS,
        std::max(1u, std::thread::hardware_concurrency()));

    auto server = builder.BuildAndStart();
    if (!server)
        throw std::runtime_error("Failed to start gRPC server on " + cfg.listen_addr);

    server->Wait();
}

} // namespace vecdb
