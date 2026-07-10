#pragma once

// ──────────────────────────────────────────────────────────────────────────────
//  AuthMiddleware — Phase 6
//
//  Validates bearer tokens from gRPC request metadata.
//  Tokens are stored in a simple in-memory set (extendable to Redis/DB).
//
//  Usage:
//    AuthMiddleware auth({"token-abc", "token-xyz"});
//    grpc::Status s = auth.check(context);
//    if (!s.ok()) return s;
// ──────────────────────────────────────────────────────────────────────────────

#include <string>
#include <unordered_set>

#include <grpcpp/grpcpp.h>

namespace vecdb {

class AuthMiddleware {
public:
    // `tokens`: set of valid bearer tokens
    explicit AuthMiddleware(std::unordered_set<std::string> tokens);

    // Check the "authorization" metadata on `ctx`.
    // Returns OK if valid, UNAUTHENTICATED otherwise.
    grpc::Status check(grpc::ServerContext* ctx) const;

    // Check a raw header value directly (e.g. "Bearer <token>").
    // Used in unit tests without a real gRPC context.
    grpc::Status check_value(const std::string& header_value) const;

    // Add/remove tokens at runtime
    void add_token(const std::string& token);
    void remove_token(const std::string& token);

    // For testing: bypass auth entirely
    void set_disabled(bool disabled) { disabled_ = disabled; }

private:
    std::unordered_set<std::string> tokens_;
    bool disabled_ = false;
};

} // namespace vecdb
