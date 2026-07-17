#include "http_api.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace vecdb {
namespace {

constexpr int kBacklog = 16;
constexpr size_t kMaxRequestBytes = 1 << 20;

std::string status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default:  return "OK";
    }
}

std::string now_iso8601() {
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now();
    const auto tt = Clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string read_request(int fd) {
    std::string data;
    data.reserve(4096);
    char buf[4096];
    while (data.find("\r\n\r\n") == std::string::npos && data.size() < kMaxRequestBytes) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, buf + n);
    }
    return data;
}

std::string content_length_body(int fd, const std::string& request, size_t header_end) {
    std::string body = request.substr(header_end + 4);
    const auto headers = request.substr(0, header_end);
    const auto cl_pos = headers.find("Content-Length:");
    if (cl_pos == std::string::npos) return body;

    size_t value_start = cl_pos + std::strlen("Content-Length:");
    while (value_start < headers.size() && std::isspace(static_cast<unsigned char>(headers[value_start]))) {
        ++value_start;
    }
    size_t value_end = value_start;
    while (value_end < headers.size() && std::isdigit(static_cast<unsigned char>(headers[value_end]))) {
        ++value_end;
    }

    size_t expected = 0;
    try {
        expected = static_cast<size_t>(std::stoul(headers.substr(value_start, value_end - value_start)));
    } catch (...) {
        return body;
    }

    while (body.size() < expected && body.size() < kMaxRequestBytes) {
        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, buf + n);
    }
    if (body.size() > expected) body.resize(expected);
    return body;
}

std::string json_error(const std::string& msg) {
    return std::string{"{\"error\":\""} + HttpApiServer::escape_json(msg) + "\"}";
}

std::string json_ok(const std::string& body, const std::string& cors_origin, int code = 200) {
    return HttpApiServer::make_response(code, body, cors_origin);
}

} // namespace

HttpApiServer::HttpApiServer(ShardManager& shard_mgr,
                             AuthMiddleware& auth,
                             RateLimiter& rate_limiter,
                             HttpApiConfig cfg)
    : shard_mgr_(shard_mgr)
    , auth_(auth)
    , rate_limiter_(rate_limiter)
    , cfg_(std::move(cfg))
    , start_time_(std::chrono::steady_clock::now()) {}

HttpApiServer::~HttpApiServer() {
    stop();
    wait();
}

void HttpApiServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    server_thread_ = std::thread([this] {
        try {
            serve_loop();
        } catch (const std::exception&) {
            running_.store(false);
        }
    });
}

void HttpApiServer::stop() {
    running_.store(false);
    std::lock_guard<std::mutex> lock(mu_);
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void HttpApiServer::wait() {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HttpApiServer::serve_loop() {
    std::string host = "0.0.0.0";
    int port = 8080;
    const auto colon = cfg_.listen_addr.rfind(':');
    if (colon != std::string::npos) {
        host = cfg_.listen_addr.substr(0, colon);
        port = std::stoi(cfg_.listen_addr.substr(colon + 1));
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("HTTP API: failed to create socket");
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        listen_fd_ = fd;
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (host == "0.0.0.0" || host == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("HTTP API: invalid listen address: " + cfg_.listen_addr);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error("HTTP API: bind failed on " + cfg_.listen_addr + ": " + std::string(std::strerror(errno)));
    }

    if (::listen(fd, kBacklog) != 0) {
        throw std::runtime_error("HTTP API: listen failed on " + cfg_.listen_addr + ": " + std::string(std::strerror(errno)));
    }

    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (!running_.load()) break;
            continue;
        }

        char ip_buf[INET_ADDRSTRLEN] = "0.0.0.0";
        ::inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        const std::string peer_ip = ip_buf;

        std::thread(&HttpApiServer::handle_client, this, client_fd, peer_ip).detach();
    }
}

void HttpApiServer::handle_client(int client_fd, const std::string& peer_ip) {
    const std::string request = read_request(client_fd);
    if (request.empty()) {
        ::close(client_fd);
        return;
    }

    const auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        const auto resp = make_response(400, json_error("bad request"), cfg_.cors_origin);
        ::send(client_fd, resp.data(), resp.size(), 0);
        ::close(client_fd);
        return;
    }

    const auto line_end = request.find("\r\n");
    if (line_end == std::string::npos) {
        const auto resp = make_response(400, json_error("bad request"), cfg_.cors_origin);
        ::send(client_fd, resp.data(), resp.size(), 0);
        ::close(client_fd);
        return;
    }

    std::istringstream iss(request.substr(0, line_end));
    std::string method, path, version;
    iss >> method >> path >> version;
    const auto headers = parse_headers(request.substr(0, header_end));
    const std::string body = content_length_body(client_fd, request, header_end);
    const std::string resp = process_request(request, peer_ip);

    ::send(client_fd, resp.data(), resp.size(), 0);
    ::close(client_fd);
}

std::string HttpApiServer::process_request(const std::string& request_text, const std::string& peer_ip) {
    const auto header_end = request_text.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return make_response(400, json_error("bad request"), cfg_.cors_origin);
    }

    const auto line_end = request_text.find("\r\n");
    if (line_end == std::string::npos) {
        return make_response(400, json_error("bad request"), cfg_.cors_origin);
    }

    std::istringstream iss(request_text.substr(0, line_end));
    std::string method, path, version;
    iss >> method >> path >> version;
    const auto headers = parse_headers(request_text.substr(0, header_end));
    const std::string body = request_text.substr(header_end + 4);

    if (method == "OPTIONS") {
        return make_options(cfg_.cors_origin);
    }

    if (path == "/api/health") {
        if (method != "GET") return make_response(405, json_error("method not allowed"), cfg_.cors_origin);
        return json_ok(handle_health(), cfg_.cors_origin);
    }

    if (path == "/api/metrics") {
        if (method != "GET") return make_response(405, json_error("method not allowed"), cfg_.cors_origin);
        return json_ok(handle_metrics(), cfg_.cors_origin);
    }

    if (path == "/api/info") {
        if (method != "GET") return make_response(405, json_error("method not allowed"), cfg_.cors_origin);
        if (!authorize(headers)) return make_response(401, json_error("unauthorized"), cfg_.cors_origin);
        if (!rate_limit(headers, peer_ip)) return make_response(429, json_error("rate limit exceeded"), cfg_.cors_origin);
        return json_ok(handle_info(), cfg_.cors_origin);
    }

    if (path == "/api/search") {
        if (method != "POST") return make_response(405, json_error("method not allowed"), cfg_.cors_origin);
        if (!authorize(headers)) return make_response(401, json_error("unauthorized"), cfg_.cors_origin);
        if (!rate_limit(headers, peer_ip)) return make_response(429, json_error("rate limit exceeded"), cfg_.cors_origin);
        return json_ok(handle_search(body), cfg_.cors_origin);
    }

    if (path == "/api/insert") {
        if (method != "POST") return make_response(405, json_error("method not allowed"), cfg_.cors_origin);
        if (!authorize(headers)) return make_response(401, json_error("unauthorized"), cfg_.cors_origin);
        if (!rate_limit(headers, peer_ip)) return make_response(429, json_error("rate limit exceeded"), cfg_.cors_origin);
        return json_ok(handle_insert(body), cfg_.cors_origin);
    }

    if (path == "/api/delete") {
        if (method != "POST") return make_response(405, json_error("method not allowed"), cfg_.cors_origin);
        if (!authorize(headers)) return make_response(401, json_error("unauthorized"), cfg_.cors_origin);
        if (!rate_limit(headers, peer_ip)) return make_response(429, json_error("rate limit exceeded"), cfg_.cors_origin);
        return make_response(501, handle_delete(body), cfg_.cors_origin);
    }

    return make_response(404, json_error("not found"), cfg_.cors_origin);
}

std::string HttpApiServer::handle_health() const {
    return std::string{"{"}
        + "\"status\":\"ok\"," 
        + "\"service\":\"vecdb-http\"," 
        + "\"time\":\"" + now_iso8601() + "\"}";
}

std::string HttpApiServer::handle_info() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"total_vectors\":" << shard_mgr_.total_vectors() << ",";
    oss << "\"shard_count\":" << shard_mgr_.shard_count() << ",";
    oss << "\"dim\":" << shard_mgr_.cfg_dim() << ",";
    oss << "\"shards\":[";
    const auto dist = shard_mgr_.load_distribution();
    for (uint32_t s = 0; s < shard_mgr_.shard_count(); ++s) {
        if (s) oss << ",";
        oss << "{";
        oss << "\"shard_id\":" << s << ",";
        oss << "\"vector_count\":" << dist[s] << ",";
        oss << "\"status\":\"" << (shard_mgr_.shard_healthy(s) ? "healthy" : "degraded") << "\"";
        oss << "}";
    }
    oss << "]}";
    return oss.str();
}

std::string HttpApiServer::handle_insert(const std::string& body) {
    uint64_t vector_id = 0;
    std::vector<float> vector;
    if (!parse_json_u64(body, "vector_id", vector_id) || !parse_json_number_array(body, "vector", vector)) {
        return json_error("invalid insert payload");
    }
    if (static_cast<int>(vector.size()) != shard_mgr_.cfg_dim()) {
        return json_error("vector dimension mismatch");
    }

    const auto [shard_id, node_id] = shard_mgr_.insert(vector_id, vector.data());
    record_insert();
    std::ostringstream oss;
    oss << "{";
    oss << "\"vector_id\":" << vector_id << ",";
    oss << "\"shard_id\":" << shard_id << ",";
    oss << "\"node_id\":" << node_id << "}";
    return oss.str();
}

std::string HttpApiServer::handle_search(const std::string& body) {
    int top_k = 10;
    int ef_search = 0;
    std::vector<float> query;
    if (!parse_json_number_array(body, "query", query) &&
        !parse_json_number_array(body, "vector", query)) {
        return json_error("invalid search payload");
    }
    (void)parse_json_int(body, "top_k", top_k);
    (void)parse_json_int(body, "ef_search", ef_search);

    if (top_k <= 0) return json_error("top_k must be > 0");
    if (static_cast<int>(query.size()) != shard_mgr_.cfg_dim()) {
        return json_error("vector dimension mismatch");
    }

    const auto t0 = std::chrono::high_resolution_clock::now();
    const auto result = shard_mgr_.search(query.data(), top_k, ef_search);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    record_search_latency(static_cast<double>(latency_us));

    std::ostringstream oss;
    oss << "{";
    oss << "\"latency_us\":" << latency_us << ",";
    oss << "\"degraded\":" << (result.degraded ? "true" : "false") << ",";
    oss << "\"shards_ok\":" << result.shards_ok << ",";
    oss << "\"shards_err\":" << result.shards_err << ",";
    oss << "\"results\":[";
    for (size_t i = 0; i < result.results.size(); ++i) {
        if (i) oss << ",";
        const auto& r = result.results[i];
        oss << "{";
        oss << "\"node_id\":" << r.node_id << ",";
        oss << "\"distance\":" << r.dist;
        oss << "}";
    }
    oss << "]}";
    return oss.str();
}

std::string HttpApiServer::handle_metrics() const {
    const auto now = std::chrono::system_clock::now();
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    uint64_t total_inserts = 0;
    uint64_t total_searches = 0;
    std::vector<double> latencies;
    {
        std::lock_guard<std::mutex> lock(metrics_mu_);
        total_inserts = total_inserts_;
        total_searches = total_searches_;
        latencies.assign(search_latencies_us_.begin(), search_latencies_us_.end());
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
    const double insert_rate = elapsed > 0.0 ? static_cast<double>(total_inserts) / elapsed : 0.0;
    const double search_rate = elapsed > 0.0 ? static_cast<double>(total_searches) / elapsed : 0.0;
    const double p50 = percentile(latencies, 50.0);
    const double p95 = percentile(latencies, 95.0);
    const double p99 = percentile(latencies, 99.0);

    std::ostringstream oss;
    oss << "{";
    oss << "\"ts\":" << ts << ",";
    oss << "\"total_vectors\":" << shard_mgr_.total_vectors() << ",";
    oss << "\"insert_rate\":" << insert_rate << ",";
    oss << "\"search_rate\":" << search_rate << ",";
    oss << "\"p50_us\":" << p50 << ",";
    oss << "\"p95_us\":" << p95 << ",";
    oss << "\"p99_us\":" << p99 << ",";
    oss << "\"total_inserts\":" << total_inserts << ",";
    oss << "\"total_searches\":" << total_searches << ",";
    oss << "\"shards\":[";
    const auto dist = shard_mgr_.load_distribution();
    for (uint32_t s = 0; s < shard_mgr_.shard_count(); ++s) {
        if (s) oss << ",";
        oss << "{";
        oss << "\"id\":" << s << ",";
        oss << "\"vectors\":" << dist[s] << ",";
        oss << "\"status\":\"" << (shard_mgr_.shard_healthy(s) ? "healthy" : "degraded") << "\"";
        oss << "}";
    }
    oss << "]}";
    return oss.str();
}

std::string HttpApiServer::handle_delete(const std::string&) const {
    return json_error("delete is not implemented in vecdb yet");
}

void HttpApiServer::record_insert() {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    ++total_inserts_;
}

void HttpApiServer::record_search_latency(double latency_us) {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    ++total_searches_;
    search_latencies_us_.push_back(latency_us);
    if (search_latencies_us_.size() > 256) {
        search_latencies_us_.pop_front();
    }
}

bool HttpApiServer::authorize(const std::unordered_map<std::string, std::string>& headers) const {
    if (auth_.is_disabled()) return true;
    const std::string value = get_header(headers, "authorization");
    if (value.empty()) return false;
    return auth_.check_value(value).ok();
}

bool HttpApiServer::rate_limit(const std::unordered_map<std::string, std::string>& headers,
                               const std::string& peer_ip) const {
    const std::string auth_value = get_header(headers, "authorization");
    const std::string client_id = auth_value.empty() ? peer_ip : auth_value;
    return rate_limiter_.check(client_id).ok();
}

std::string HttpApiServer::trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string HttpApiServer::lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string HttpApiServer::escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> HttpApiServer::parse_headers(const std::string& header_block) {
    std::unordered_map<std::string, std::string> headers;
    std::istringstream iss(header_block);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (first) {
            first = false;
            continue;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return headers;
}

std::string HttpApiServer::get_header(const std::unordered_map<std::string, std::string>& headers,
                                      const std::string& key) {
    const auto it = headers.find(lower(key));
    return it == headers.end() ? std::string{} : it->second;
}

bool HttpApiServer::parse_json_int(const std::string& body,
                                   const std::string& key,
                                   int& out) {
    const std::string needle = "\"" + key + "\"";
    const auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    const auto colon = body.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    const auto end = body.find_first_of(",}\r\n", colon + 1);
    try {
        out = std::stoi(trim(body.substr(colon + 1, end - colon - 1)));
        return true;
    } catch (...) {
        return false;
    }
}

bool HttpApiServer::parse_json_u64(const std::string& body,
                                   const std::string& key,
                                   uint64_t& out) {
    const std::string needle = "\"" + key + "\"";
    const auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    const auto colon = body.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    const auto end = body.find_first_of(",}\r\n", colon + 1);
    try {
        out = std::stoull(trim(body.substr(colon + 1, end - colon - 1)));
        return true;
    } catch (...) {
        return false;
    }
}

bool HttpApiServer::parse_json_number_array(const std::string& body,
                                            const std::string& key,
                                            std::vector<float>& out) {
    const std::string needle = "\"" + key + "\"";
    const auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    const auto lb = body.find('[', pos + needle.size());
    const auto rb = body.find(']', lb == std::string::npos ? pos : lb);
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1) return false;

    std::stringstream ss(body.substr(lb + 1, rb - lb - 1));
    out.clear();
    while (ss.good()) {
        std::string token;
        if (!std::getline(ss, token, ',')) break;
        token = trim(token);
        if (token.empty()) continue;
        try {
            out.push_back(std::stof(token));
        } catch (...) {
            return false;
        }
    }
    return !out.empty();
}

double HttpApiServer::percentile(std::vector<double> values, double pct) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = (pct / 100.0) * (values.size() - 1);
    const auto lower = static_cast<size_t>(pos);
    const auto upper = std::min(values.size() - 1, lower + 1);
    const double frac = pos - static_cast<double>(lower);
    return values[lower] * (1.0 - frac) + values[upper] * frac;
}

std::string HttpApiServer::make_response(int code,
                                         const std::string& body,
                                         const std::string& cors_origin,
                                         const std::string& content_type) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << ' ' << status_text(code) << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Access-Control-Allow-Origin: " << cors_origin << "\r\n";
    oss << "Access-Control-Allow-Headers: Authorization, Content-Type\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    return oss.str();
}

std::string HttpApiServer::make_options(const std::string& cors_origin) {
    std::ostringstream oss;
    oss << "HTTP/1.1 204 No Content\r\n";
    oss << "Access-Control-Allow-Origin: " << cors_origin << "\r\n";
    oss << "Access-Control-Allow-Headers: Authorization, Content-Type\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    oss << "Access-Control-Max-Age: 86400\r\n";
    oss << "Content-Length: 0\r\n";
    oss << "Connection: close\r\n\r\n";
    return oss.str();
}

void run_http_api(ShardManager& shard_mgr,
                  AuthMiddleware& auth,
                  RateLimiter& rate_limiter,
                  const HttpApiConfig& cfg) {
    HttpApiServer server(shard_mgr, auth, rate_limiter, cfg);
    server.start();
    server.wait();
}

} // namespace vecdb
