#pragma once

// ──────────────────────────────────────────────────────────────────────────────
//  VectorDBServiceImpl — Phase 6
//
//  Implements the gRPC VectorDB service (generated from vecdb.proto).
//  Sits on top of ShardManager — translates protobuf RPCs to shard operations.
//
//  Every RPC:
//    1. AuthMiddleware::check() — validates bearer token
//    2. RateLimiter::check()   — enforces per-client quota
//    3. Decode protobuf bytes → raw float* 
//    4. Call ShardManager::insert() or search()
//    5. Encode result → protobuf response
//
//  Thread model:
//    gRPC creates a completion queue with one thread per CPU core.
//    Each RPC handler runs on these threads — no additional dispatch needed.
//    ShardManager handles its own locking internally.
// ──────────────────────────────────────────────────────────────────────────────

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "src/proto/vecdb.grpc.pb.h"
#include "auth_middleware.hpp"
#include "rate_limiter.hpp"
#include "src/shard/shard_manager.hpp"

namespace vecdb {

class VectorDBServiceImpl final : public vecdb::VectorDB::Service {
public:
    VectorDBServiceImpl(ShardManager&    shard_mgr,
                        AuthMiddleware&  auth,
                        RateLimiter&     rate_limiter);

    // ── RPC handlers ──────────────────────────────────────────────────────────

    grpc::Status Insert(grpc::ServerContext*       ctx,
                        const vecdb::InsertRequest* req,
                        vecdb::InsertResponse*      resp) override;

    grpc::Status BulkInsert(grpc::ServerContext*                              ctx,
                             grpc::ServerReader<vecdb::InsertRequest>*         reader,
                             vecdb::BulkInsertResponse*                        resp) override;

    grpc::Status Search(grpc::ServerContext*       ctx,
                        const vecdb::SearchRequest* req,
                        vecdb::SearchResponse*      resp) override;

    grpc::Status Delete(grpc::ServerContext*       ctx,
                        const vecdb::DeleteRequest* req,
                        vecdb::DeleteResponse*      resp) override;

    grpc::Status GetInfo(grpc::ServerContext*      ctx,
                         const vecdb::InfoRequest*  req,
                         vecdb::InfoResponse*       resp) override;

private:
    // Decode raw bytes → float vector. Returns false if size is wrong.
    bool decode_vector(const std::string& bytes,
                       std::vector<float>& out) const;

    // Common auth + rate-limit check
    grpc::Status gate(grpc::ServerContext* ctx);

    ShardManager&   shard_mgr_;
    AuthMiddleware& auth_;
    RateLimiter&    rate_limiter_;
    int             dim_;
};

// ── Server lifecycle ──────────────────────────────────────────────────────────

struct ServerConfig {
    std::string listen_addr   = "0.0.0.0:50051";
    bool        use_tls       = false;
    std::string tls_cert_path;
    std::string tls_key_path;
};

// Start the gRPC server (blocks until server shuts down or Shutdown() called).
void run_server(ShardManager&       shard_mgr,
                AuthMiddleware&     auth,
                RateLimiter&        rate_limiter,
                const ServerConfig& cfg);

} // namespace vecdb
