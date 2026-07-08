#pragma once
#include <cstdint>
#include <string>

// ──────────────────────────────────────────────────────────────────────────────
//  Snapshot — Phase 4
//
//  Persists the complete index state to disk so it survives a clean shutdown
//  and can be reloaded in milliseconds (mmap is O(1); OS handles paging lazily).
//
//  Two files are written per snapshot:
//
//  1. <prefix>.seg   — MmapStore segment: raw float32 vectors (zero-copy)
//  2. <prefix>.graph — JSON adjacency lists for the HNSW graph + metadata
//
//  The .seg file uses the MmapStore binary format (64-byte header + float data).
//  The .graph file is a simple JSON structure:
//
//    {
//      "version": 1,
//      "dim": 768,
//      "vec_count": 100000,
//      "M": 16,
//      "ef_construction": 200,
//      "max_layers": 6,
//      "entry_point": 12345,
//      "max_layer": 4,
//      "adj": [                   // array of vec_count objects
//        { "l": [[n,n,...], [n,...]] },   // per-node, per-layer neighbours
//        ...
//      ]
//    }
//
//  Why JSON for the graph (not binary)?
//    - Pointer-based pmr structures can't be mmap'd directly
//    - JSON is self-describing: easy to inspect, diff, and debug
//    - Graph serialisation is done once at shutdown; latency doesn't matter
//    - For Phase 5+, this can be replaced with FlatBuffers for speed
//
//  After a successful save(), the WAL can be truncated (Phase 3 integration):
//    wal.truncate(snapshot_lsn)
// ──────────────────────────────────────────────────────────────────────────────

#include <string>

// Forward declarations — avoids pulling in heavy headers
namespace vecdb {
    class HNSWIndex;
    class WAL;
}

namespace vecdb::storage {

struct SnapshotPaths {
    std::string seg;    // path to .seg  (MmapStore binary)
    std::string graph;  // path to .graph (JSON adjacency)
};

// Build snapshot file paths from a directory and shard id
SnapshotPaths make_paths(const std::string& dir, uint32_t shard_id = 0);

// ── Save ─────────────────────────────────────────────────────────────────────
//
// Write the complete index state to disk.
// `idx`  — index to snapshot (read lock held internally)
// `paths` — where to write (use make_paths() to generate)
//
// After save() returns, both files are fsync'd.
// Throws on any I/O error.
void save(const HNSWIndex& idx, const SnapshotPaths& paths);

// ── Load ─────────────────────────────────────────────────────────────────────
//
// Load a previously saved snapshot into `idx`.
// `idx` must be freshly constructed (empty) — existing state is overwritten.
//
// After load() returns, `idx` is ready for searches.
// Throws if files are missing, corrupt, or dim/config mismatches.
void load(HNSWIndex& idx, const SnapshotPaths& paths);

// ── Save + WAL truncation (convenience) ──────────────────────────────────────
//
// Equivalent to: save(idx, paths); wal.truncate(wal.current_lsn());
// Keeps WAL and snapshot in sync.
void save_and_truncate_wal(const HNSWIndex& idx,
                            const SnapshotPaths& paths,
                            WAL& wal);

} // namespace vecdb::storage