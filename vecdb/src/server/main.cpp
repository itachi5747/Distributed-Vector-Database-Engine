// // vecdb — Distributed Vector Database Engine
// // Phase 6 — gRPC + HTTP API Server Entry Point

// // Usage:
// //   ./vecdb_server [--addr 0.0.0.0:50051] [--http-addr 0.0.0.0:8080]
// //                 [--shards 3] [--dim 768]
// //                 [--data-dir /data] [--token <bearer-token>]
// //                 [--no-auth] [--rate 100] [--load]

// #include <iostream>
// #include <string>
// #include <unordered_set>
// #include <vector>

// #include "src/hnsw/index.hpp"
// #include "src/shard/shard_manager.hpp"
// #include "src/server/auth_middleware.hpp"
// #include "src/server/rate_limiter.hpp"
// #include "src/server/grpc_server.hpp"
// #include "src/server/http_api.hpp"
// #include "src/simd/distance.hpp"

// static void print_usage(const char* prog) {
//     std::cout << "Usage: " << prog << " [options]\n"
//               << "  --addr       <host:port>  gRPC listen address (default: 0.0.0.0:50051)\n"
//               << "  --http-addr  <host:port>  HTTP listen address (default: 0.0.0.0:8080)\n"
//               << "  --shards     <N>          Number of shards (default: 3)\n"
//               << "  --dim        <D>          Vector dimension (default: 768)\n"
//               << "  --data-dir   <path>       Data directory (default: ./data)\n"
//               << "  --token      <token>      Bearer token for auth (repeatable)\n"
//               << "  --no-auth                 Disable authentication\n"
//               << "  --rate       <N>          Requests/sec per client (default: 100)\n"
//               << "  --load                    Load snapshots from data-dir on startup\n";
// }

// int main(int argc, char* argv[]) {
//     std::string grpc_addr = "0.0.0.0:50051";
//     std::string http_addr = "0.0.0.0:8080";
//     int         shards    = 3;
//     int         dim       = 768;
//     std::string data_dir  = "./data";
//     double      rate      = 100.0;
//     bool        no_auth   = false;
//     bool        load      = false;
//     std::vector<std::string> tokens;

//     for (int i = 1; i < argc; ++i) {
//         std::string arg = argv[i];
//         if      (arg == "--addr"      && i + 1 < argc) grpc_addr = argv[++i];
//         else if (arg == "--http-addr" && i + 1 < argc) http_addr = argv[++i];
//         else if (arg == "--shards"    && i + 1 < argc) shards    = std::stoi(argv[++i]);
//         else if (arg == "--dim"       && i + 1 < argc) dim       = std::stoi(argv[++i]);
//         else if (arg == "--data-dir"  && i + 1 < argc) data_dir  = argv[++i];
//         else if (arg == "--token"     && i + 1 < argc) tokens.push_back(argv[++i]);
//         else if (arg == "--rate"      && i + 1 < argc) rate      = std::stod(argv[++i]);
//         else if (arg == "--no-auth")                   no_auth   = true;
//         else if (arg == "--load")                      load      = true;
//         else if (arg == "--help" || arg == "-h")      { print_usage(argv[0]); return 0; }
//     }

//     vecdb::simd::init();
//     std::cout << "[vecdb] SIMD: " << vecdb::simd::active_isa_name() << "\n";

//     vecdb::HNSWConfig cfg;
//     cfg.dim             = dim;
//     cfg.M               = 16;
//     cfg.M_max0          = 32;
//     cfg.ef_construction = 200;
//     cfg.ef_search       = 50;
//     cfg.arena_bytes     = 512ULL * 1024 * 1024;

//     std::cout << "[vecdb] Initialising " << shards << " shards, dim=" << dim << "\n";
//     vecdb::ShardManager mgr(cfg, static_cast<uint32_t>(shards), data_dir);

//     if (load) {
//         std::cout << "[vecdb] Loading snapshots from " << data_dir << "...\n";
//         mgr.load_all();
//         std::cout << "[vecdb] Loaded " << mgr.total_vectors() << " vectors\n";
//     }

//     std::unordered_set<std::string> token_set(tokens.begin(), tokens.end());
//     vecdb::AuthMiddleware auth(token_set);
//     if (no_auth || tokens.empty()) {
//         auth.set_disabled(true);
//         std::cout << "[vecdb] Auth: DISABLED\n";
//     } else {
//         std::cout << "[vecdb] Auth: enabled (" << tokens.size() << " token(s))\n";
//     }

//     vecdb::RateLimiter rl(rate * 2.0, rate);
//     std::cout << "[vecdb] Rate limit: " << rate << " req/s per client\n";

//     vecdb::ServerConfig grpc_cfg;
//     grpc_cfg.listen_addr = grpc_addr;

//     vecdb::HttpApiConfig http_cfg;
//     http_cfg.listen_addr = http_addr;
//     http_cfg.cors_origin = "*";

//     std::cout << "[vecdb] gRPC listening on " << grpc_addr << "\n";
//     std::cout << "[vecdb] HTTP  listening on " << http_addr << "\n";
//     std::cout << "[vecdb] Ready — press Ctrl+C to stop\n";

//     vecdb::HttpApiServer http_server(mgr, auth, rl, http_cfg);
//     http_server.start();

//     run_server(mgr, auth, rl, grpc_cfg);

//     http_server.stop();
//     http_server.wait();
//     return 0;
// }
// vecdb — Distributed Vector Database Engine
// Phase 6 — HTTP API Server Entry Point
//
// Usage:
//   ./vecdb_server [--http-addr 0.0.0.0:8080] [--port 8080]
//                  [--shards 3] [--dim 768]
//                  [--data-dir /data] [--load]

#include <iostream>
#include <string>

#include "src/hnsw/index.hpp"
#include "src/shard/shard_manager.hpp"
#include "src/server/http_api.hpp"
#include "src/simd/distance.hpp"

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --port       <N>          HTTP listen port (default: 8080)\n"
              << "  --shards     <N>          Number of shards (default: 3)\n"
              << "  --dim        <D>          Vector dimension (default: 768)\n"
              << "  --data-dir   <path>       Data directory (default: ./data)\n"
              << "  --load                    Load snapshots from data-dir on startup\n";
}

int main(int argc, char* argv[]) {
    uint16_t    port     = 8080;
    int         shards   = 3;
    int         dim      = 768;
    std::string data_dir = "./data";
    bool        load     = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--port"     && i + 1 < argc) port     = (uint16_t)std::stoi(argv[++i]);
        else if (arg == "--shards"   && i + 1 < argc) shards   = std::stoi(argv[++i]);
        else if (arg == "--dim"      && i + 1 < argc) dim      = std::stoi(argv[++i]);
        else if (arg == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--load")                     load     = true;
        else if (arg == "--help" || arg == "-h")     { print_usage(argv[0]); return 0; }
    }

    vecdb::simd::init();
    std::cout << "[vecdb] SIMD: " << vecdb::simd::active_isa_name() << "\n";

    vecdb::HNSWConfig cfg;
    cfg.dim             = dim;
    cfg.M               = 16;
    cfg.M_max0          = 32;
    cfg.ef_construction = 200;
    cfg.ef_search       = 50;
    cfg.arena_bytes     = 512ULL * 1024 * 1024;

    std::cout << "[vecdb] Initialising " << shards << " shards, dim=" << dim << "\n";
    vecdb::ShardManager mgr(cfg, static_cast<uint32_t>(shards), data_dir);

    if (load) {
        std::cout << "[vecdb] Loading snapshots from " << data_dir << "...\n";
        mgr.load_all();
        std::cout << "[vecdb] Loaded " << mgr.total_vectors() << " vectors\n";
    }

    vecdb::MetricsStore metrics;
    vecdb::HttpApiServer http_server(mgr, metrics, port);

    std::cout << "[vecdb] HTTP listening on port " << port << "\n";
    std::cout << "[vecdb] Ready — press Ctrl+C to stop\n";

    http_server.start();

    // Block the main thread; run() spins its own loop until stop() is called.
    std::cout << "[vecdb] Press Enter to shut down...\n";
    std::cin.get();

    http_server.stop();
    return 0;
}