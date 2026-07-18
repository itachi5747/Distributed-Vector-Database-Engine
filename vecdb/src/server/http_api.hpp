#pragma once

// ──────────────────────────────────────────────────────────────────────────────
//  HttpApiServer — Phase 7 (updated with Insert + Graph Inspector)
//
//  Endpoints:
//    GET  /api/health          → {"status":"ok"}
//    GET  /api/info            → shard counts, vector totals
//    GET  /api/metrics         → latency percentiles + throughput (1s rolling)
//    POST /api/search          → {"query":[f,...], "top_k":N}
//    POST /api/insert          → {"vector":[f,...], "vector_id":N}  (NEW)
//    GET  /api/node?id=N       → neighbours at each HNSW layer     (NEW)
//
//  CORS headers on every response so http://localhost:5173 can call freely.
// ──────────────────────────────────────────────────────────────────────────────

#include <string>
#include <atomic>
#include <thread>
#include <cstdint>

namespace vecdb {

class ShardManager;

// ── MetricsStore ──────────────────────────────────────────────────────────────
struct MetricsStore {
    std::atomic<uint64_t> total_inserts{0};
    std::atomic<uint64_t> total_searches{0};
    std::atomic<uint64_t> insert_latency_us_sum{0};
    std::atomic<uint64_t> search_latency_us_sum{0};
    std::atomic<uint64_t> search_p50_us{0};
    std::atomic<uint64_t> search_p95_us{0};
    std::atomic<uint64_t> search_p99_us{0};
    std::atomic<uint64_t> inserts_last_sec{0};
    std::atomic<uint64_t> searches_last_sec{0};
    std::atomic<uint64_t> inserts_window{0};
    std::atomic<uint64_t> searches_window{0};

    static constexpr int N_BUCKETS = 8;
    std::atomic<uint64_t> latency_buckets[N_BUCKETS]{};

    void record_search(uint64_t latency_us);
    void record_insert(uint64_t latency_us);
    void compute_percentiles();
};

// ── HttpApiServer ─────────────────────────────────────────────────────────────
class HttpApiServer {
public:
    HttpApiServer(ShardManager& mgr, MetricsStore& metrics, uint16_t port = 8080);
    ~HttpApiServer();

    void start();
    void stop();

private:
    void run();
    void handle_client(int client_fd);

    std::string handle_health();
    std::string handle_info();
    std::string handle_metrics();
    std::string handle_search(const std::string& body);
    std::string handle_insert(const std::string& body);      // NEW
    std::string handle_node(const std::string& query_str);   // NEW

    std::string ok_response(const std::string& json);
    std::string err_response(int code, const std::string& msg);
    std::string parse_body(const std::string& request);
    std::string parse_path(const std::string& request);
    std::string parse_query(const std::string& request);     // NEW
    std::string parse_method(const std::string& request);

    ShardManager&     mgr_;
    MetricsStore&     metrics_;
    uint16_t          port_;
    int               server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace vecdb
