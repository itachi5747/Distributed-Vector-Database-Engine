#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "auth_middleware.hpp"
#include "rate_limiter.hpp"
#include "src/shard/shard_manager.hpp"

namespace vecdb {

struct HttpApiConfig {
    std::string listen_addr = "0.0.0.0:8080";
    std::string cors_origin = "*";
};

struct HttpApiServer {
    HttpApiServer(ShardManager& shard_mgr,
                  AuthMiddleware& auth,
                  RateLimiter& rate_limiter,
                  HttpApiConfig cfg = {});
    ~HttpApiServer();

    HttpApiServer(const HttpApiServer&) = delete;
    HttpApiServer& operator=(const HttpApiServer&) = delete;

    void start();
    void stop();
    void wait();

    static std::string escape_json(const std::string& s);
    static std::string make_response(int code,
                                     const std::string& body,
                                     const std::string& cors_origin,
                                     const std::string& content_type = "application/json");
    static std::string make_options(const std::string& cors_origin);

private:
    void serve_loop();
    void handle_client(int client_fd, const std::string& peer_ip);
    std::string process_request(const std::string& request_text, const std::string& peer_ip);

    std::string handle_health() const;
    std::string handle_metrics() const;
    std::string handle_info() const;
    std::string handle_insert(const std::string& body);
    std::string handle_search(const std::string& body);
    std::string handle_delete(const std::string& body) const;

    void record_insert();
    void record_search_latency(double latency_us);

    bool authorize(const std::unordered_map<std::string, std::string>& headers) const;
    bool rate_limit(const std::unordered_map<std::string, std::string>& headers,
                    const std::string& peer_ip) const;

    static std::string trim(const std::string& s);
    static std::string lower(std::string s);
    static std::unordered_map<std::string, std::string> parse_headers(const std::string& header_block);
    static std::string get_header(const std::unordered_map<std::string, std::string>& headers,
                                  const std::string& key);
    static bool parse_json_int(const std::string& body, const std::string& key, int& out);
    static bool parse_json_u64(const std::string& body, const std::string& key, uint64_t& out);
    static bool parse_json_number_array(const std::string& body,
                                        const std::string& key,
                                        std::vector<float>& out);
    static double percentile(std::vector<double> values, double pct);

    ShardManager& shard_mgr_;
    AuthMiddleware& auth_;
    RateLimiter& rate_limiter_;
    HttpApiConfig cfg_;
    std::chrono::steady_clock::time_point start_time_;

    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::thread server_thread_;

    mutable std::mutex metrics_mu_;
    uint64_t total_inserts_ = 0;
    uint64_t total_searches_ = 0;
    std::deque<double> search_latencies_us_;
};

void run_http_api(ShardManager& shard_mgr,
                  AuthMiddleware& auth,
                  RateLimiter& rate_limiter,
                  const HttpApiConfig& cfg = {});

} // namespace vecdb
