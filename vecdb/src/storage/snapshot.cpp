#include "snapshot.hpp"
#include "mmap_store.hpp"
#include "../hnsw/index.hpp"
#include "../wal/wal.hpp"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ── Minimal JSON writer/reader (no external dependency) ───────────────────────
// We write JSON manually (straightforward for our fixed schema).
// For reading, we use a simple recursive-descent parser.

namespace vecdb::storage {

// ── Path helpers ──────────────────────────────────────────────────────────────

SnapshotPaths make_paths(const std::string& dir, uint32_t shard_id) {
    std::string base = dir + "/shard_" + std::to_string(shard_id);
    return { base + ".seg", base + ".graph" };
}

// ── JSON writer helpers ───────────────────────────────────────────────────────

static void write_json_graph(const std::string& path,
                              const HNSWIndex& idx) {
    const uint32_t N       = idx.size();
    const auto&    cfg     = idx.config();
    const int      max_l   = idx.max_layer_stored();
    const uint32_t ep      = idx.entry_point_stored();

    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) throw std::runtime_error("Snapshot: cannot open graph file: " + path);

    f << "{\n";
    f << "  \"version\": 1,\n";
    f << "  \"dim\": "             << cfg.dim             << ",\n";
    f << "  \"vec_count\": "       << N                   << ",\n";
    f << "  \"M\": "               << cfg.M               << ",\n";
    f << "  \"M_max0\": "          << cfg.M_max0          << ",\n";
    f << "  \"ef_construction\": " << cfg.ef_construction << ",\n";
    f << "  \"ef_search\": "       << cfg.ef_search       << ",\n";
    f << "  \"max_layers\": "      << cfg.max_layers      << ",\n";
    f << "  \"entry_point\": "     << ep                  << ",\n";
    f << "  \"max_layer\": "       << max_l               << ",\n";
    f << "  \"adj\": [\n";

    for (uint32_t node = 0; node < N; ++node) {
        f << "    {\"l\":[";
        int nlayers = idx.layer_count_for(node);
        for (int l = 0; l < nlayers; ++l) {
            f << "[";
            const auto& nbrs = idx.neighbours_at(node, l);
            for (size_t i = 0; i < nbrs.size(); ++i) {
                f << nbrs[i];
                if (i + 1 < nbrs.size()) f << ",";
            }
            f << "]";
            if (l + 1 < nlayers) f << ",";
        }
        f << "]}";
        if (node + 1 < N) f << ",";
        f << "\n";
    }

    f << "  ]\n}\n";

    if (!f) throw std::runtime_error("Snapshot: write error on graph file");
    f.close();
}

// ── Minimal JSON parser for our fixed schema ──────────────────────────────────

struct JsonGraph {
    int      version;
    int      dim;
    uint32_t vec_count;
    int      M;
    int      M_max0;
    int      ef_construction;
    int      ef_search;
    int      max_layers;
    uint32_t entry_point;
    int      max_layer;
    // adj[node][layer] = list of neighbour node_ids
    std::vector<std::vector<std::vector<uint32_t>>> adj;
};

// Skip whitespace
static void skip_ws(const char*& p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
}

// Consume expected character, throw if mismatch
static void expect(const char*& p, char c) {
    skip_ws(p);
    if (*p != c)
        throw std::runtime_error(
            std::string("Snapshot JSON: expected '") + c + "' got '" + *p + "'");
    ++p;
}

// Parse a quoted string key (returns without quotes)
static std::string parse_key(const char*& p) {
    skip_ws(p);
    expect(p, '"');
    std::string key;
    while (*p && *p != '"') key += *p++;
    expect(p, '"');
    return key;
}

// Parse a JSON integer (signed 64-bit)
static int64_t parse_int(const char*& p) {
    skip_ws(p);
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    int64_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
    return neg ? -v : v;
}

// Parse one flat uint32 array: [n,n,n,...]
static std::vector<uint32_t> parse_u32_array(const char*& p) {
    std::vector<uint32_t> arr;
    skip_ws(p);
    expect(p, '[');
    skip_ws(p);
    if (*p == ']') { ++p; return arr; }
    while (true) {
        arr.push_back(static_cast<uint32_t>(parse_int(p)));
        skip_ws(p);
        if (*p == ']') { ++p; break; }
        expect(p, ',');
    }
    return arr;
}

// Parse {"l":[[...],[...], ...]}
static std::vector<std::vector<uint32_t>> parse_node_adj(const char*& p) {
    skip_ws(p);
    expect(p, '{');
    skip_ws(p);
    // expect key "l"
    std::string key = parse_key(p);
    if (key != "l") throw std::runtime_error("Snapshot JSON: expected key 'l'");
    expect(p, ':');
    skip_ws(p);
    expect(p, '[');

    std::vector<std::vector<uint32_t>> layers;
    skip_ws(p);
    if (*p == ']') { ++p; }
    else {
        while (true) {
            layers.push_back(parse_u32_array(p));
            skip_ws(p);
            if (*p == ']') { ++p; break; }
            expect(p, ',');
            skip_ws(p);
        }
    }
    expect(p, '}');
    return layers;
}

static JsonGraph parse_json_graph(const std::string& path) {
    // Read entire file into memory
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Snapshot: cannot open graph file: " + path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    const char* p = content.c_str();

    JsonGraph g{};
    expect(p, '{');

    // Parse top-level key-value pairs
    while (true) {
        skip_ws(p);
        if (*p == '}') { ++p; break; }
        if (*p == ',') { ++p; skip_ws(p); }

        std::string key = parse_key(p);
        expect(p, ':');
        skip_ws(p);

        if      (key == "version")        g.version        = (int)parse_int(p);
        else if (key == "dim")            g.dim            = (int)parse_int(p);
        else if (key == "vec_count")      g.vec_count      = (uint32_t)parse_int(p);
        else if (key == "M")              g.M              = (int)parse_int(p);
        else if (key == "M_max0")         g.M_max0         = (int)parse_int(p);
        else if (key == "ef_construction")g.ef_construction= (int)parse_int(p);
        else if (key == "ef_search")      g.ef_search      = (int)parse_int(p);
        else if (key == "max_layers")     g.max_layers     = (int)parse_int(p);
        else if (key == "entry_point")    g.entry_point    = (uint32_t)parse_int(p);
        else if (key == "max_layer")      g.max_layer      = (int)parse_int(p);
        else if (key == "adj") {
            expect(p, '[');
            skip_ws(p);
            if (*p == ']') { ++p; }
            else {
                while (true) {
                    g.adj.push_back(parse_node_adj(p));
                    skip_ws(p);
                    if (*p == ']') { ++p; break; }
                    expect(p, ',');
                    skip_ws(p);
                }
            }
        }
    }
    return g;
}

// ── save ─────────────────────────────────────────────────────────────────────

void save(const HNSWIndex& idx, const SnapshotPaths& paths) {
    const uint32_t N   = idx.size();
    const uint32_t dim = static_cast<uint32_t>(idx.dim());

    // ── 1. Write segment file (zero-copy float data) ──────────────────────────
    // Remove existing file so MmapStore::create doesn't throw
    std::remove(paths.seg.c_str());

    {
        MmapStore seg = MmapStore::create(paths.seg, dim);
        for (uint32_t i = 0; i < N; ++i) {
            seg.append(idx.get_vector(i));
        }
        seg.sync();
    } // seg file closed and sync'd here

    // ── 2. Write graph JSON ────────────────────────────────────────────────────
    write_json_graph(paths.graph, idx);

    // fsync the graph file
    {
        int fd = open(paths.graph.c_str(), O_RDONLY);
        if (fd >= 0) { fsync(fd); close(fd); }
    }
}

// ── load ─────────────────────────────────────────────────────────────────────

void load(HNSWIndex& idx, const SnapshotPaths& paths) {
    // ── 1. Parse graph JSON ────────────────────────────────────────────────────
    JsonGraph g = parse_json_graph(paths.graph);

    if (g.version != 1)
        throw std::runtime_error("Snapshot: unsupported graph version");
    if (g.dim != idx.dim())
        throw std::runtime_error("Snapshot: dim mismatch");

    // ── 2. Open segment file ───────────────────────────────────────────────────
    MmapStore seg = MmapStore::open(paths.seg);

    if (seg.dim()       != static_cast<uint32_t>(g.dim))
        throw std::runtime_error("Snapshot: seg/graph dim mismatch");
    if (seg.vec_count() != g.vec_count)
        throw std::runtime_error("Snapshot: seg/graph vec_count mismatch");

    // ── 3. Restore index state ────────────────────────────────────────────────
    idx.load_from_snapshot(seg, g.adj,
                           g.entry_point,
                           g.max_layer);
}

// ── save_and_truncate_wal ────────────────────────────────────────────────────

void save_and_truncate_wal(const HNSWIndex& idx,
                            const SnapshotPaths& paths,
                            WAL& wal) {
    save(idx, paths);
    wal.truncate(wal.current_lsn());
}

} // namespace vecdb::storage