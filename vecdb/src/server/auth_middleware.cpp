#include "auth_middleware.hpp"

namespace vecdb {

AuthMiddleware::AuthMiddleware(std::unordered_set<std::string> tokens)
    : tokens_(std::move(tokens)) {}

grpc::Status AuthMiddleware::check_value(const std::string& value) const {
    if (disabled_) return grpc::Status::OK;

    const std::string prefix = "Bearer ";
    if (value.size() <= prefix.size() ||
        value.substr(0, prefix.size()) != prefix) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Authorization must use Bearer scheme");
    }

    std::string token = value.substr(prefix.size());
    if (tokens_.find(token) == tokens_.end()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Invalid or expired token");
    }
    return grpc::Status::OK;
}

grpc::Status AuthMiddleware::check(grpc::ServerContext* ctx) const {
    if (disabled_) return grpc::Status::OK;

    const auto& meta = ctx->client_metadata();
    auto it = meta.find("authorization");
    if (it == meta.end()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Missing authorization header");
    }

    std::string value(it->second.data(), it->second.size());
    return check_value(value);
}

void AuthMiddleware::add_token(const std::string& token) {
    tokens_.insert(token);
}

void AuthMiddleware::remove_token(const std::string& token) {
    tokens_.erase(token);
}

} // namespace vecdb
