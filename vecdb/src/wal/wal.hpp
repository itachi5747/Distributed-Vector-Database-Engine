#pragma once

// ──────────────────────────────────────────────────────────────────────────────
//  Write-Ahead Log (WAL) — Phase 3
//
//  Guarantees: no inserted vector is ever lost, even if the process crashes
//  mid-insert (after the WAL write but before the HNSW graph update finishes).
//
//  Design:
//    - Append-only flat binary file, memory-mapped with MAP_SHARED
//    - Every insert writes one WAL entry BEFORE touching the HNSW index
//    - Each entry has a CRC32 checksum for torn-write detection
//    - On crash restart: replay() re-inserts every valid entry into a fresh index
//    - Periodic checkpoint() records a safe LSN; replay starts from there
//    - After a full index snapshot (Phase 4), truncate() bounds file growth
//
//  Binary entry format (little-endian):
//    [8 bytes] LSN          — log sequence number (uint64_t, monotone increasing)
//    [8 bytes] vector_id    — client-provided stable ID hash (uint64_t)
//    [4 bytes] dim          — number of floats (uint32_t)
//    [4 bytes] CRC32        — checksum of the float payload
//    [dim*4 bytes] floats   — raw IEEE-754 float32 vector data
//
//  File header (first 64 bytes of the file):
//    [4 bytes] magic        — 0x57414C21 ("WAL!")
//    [4 bytes] version      — format version (currently 1)
//    [8 bytes] checkpoint_lsn — LSN of last confirmed checkpoint
//    [4 bytes] dim          — vector dimensionality (set on first append)
//    [4 bytes] header_crc   — CRC32 of the above 20 bytes
//    [40 bytes] reserved    — zero-padded to 64 bytes
// ──────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <vector>

namespace vecdb {

// ── WAL entry as seen by callers ──────────────────────────────────────────────
struct WALEntry {
    uint64_t    lsn;        // log sequence number
    uint64_t    vector_id;  // stable external id
    uint32_t    dim;        // floats in this entry
    const float* vec;       // pointer into the mmap region (valid until remap)
};

// ── WAL class ─────────────────────────────────────────────────────────────────
class WAL {
public:
    // Open (or create) a WAL file at `path` for vectors of `dim` floats.
    // `checkpoint_every`: call msync after this many appends (0 = never auto-sync)
    WAL(const std::string& path, uint32_t dim, uint32_t checkpoint_every = 1000);
    ~WAL();

    WAL(const WAL&)            = delete;
    WAL& operator=(const WAL&) = delete;

    // ── Write path ────────────────────────────────────────────────────────────

    // Append one entry. Writes to the mmap region; may call msync if
    // checkpoint_every threshold is reached.
    // Returns the LSN assigned to this entry.
    uint64_t append(uint64_t vector_id, const float* vec);

    // Force an msync(MS_SYNC) flush and record checkpoint_lsn in the header.
    // Call explicitly after bulk inserts or before snapshotting.
    void checkpoint();

    // ── Recovery path ─────────────────────────────────────────────────────────

    // Replay all valid entries from checkpoint_lsn forward.
    // For each valid entry, calls `fn(entry)`.
    // Stops at the first entry with an invalid CRC (torn write detection).
    // Returns number of entries replayed.
    uint64_t replay(std::function<void(const WALEntry&)> fn) const;

    // ── Maintenance ───────────────────────────────────────────────────────────

    // Truncate the WAL to `keep_from_lsn` — discard all entries before that LSN.
    // Used after a full index snapshot (Phase 4) to bound file growth.
    void truncate(uint64_t keep_from_lsn);

    // ── Introspection ─────────────────────────────────────────────────────────
    uint64_t current_lsn()     const { return next_lsn_; }
    uint64_t checkpoint_lsn()  const { return checkpoint_lsn_; }
    uint32_t dim()             const { return dim_; }
    size_t   file_size_bytes() const;

private:
    // ── Internal helpers ──────────────────────────────────────────────────────
    void     open_or_create();
    void     grow(size_t new_size);          // mremap / remap file + mapping
    void     write_header();
    void     read_header();
    uint32_t crc32_vec(const float* vec, uint32_t dim) const;
    size_t   entry_size() const;             // header + float payload
    uint8_t* entry_ptr(uint64_t lsn) const; // pointer to start of entry in mmap

    // ── State ─────────────────────────────────────────────────────────────────
    std::string path_;
    uint32_t    dim_;
    uint32_t    checkpoint_every_;

    int         fd_           = -1;    // file descriptor
    uint8_t*    map_          = nullptr;
    size_t      map_size_     = 0;     // current mmap size in bytes

    uint64_t    next_lsn_         = 0;
    uint64_t    checkpoint_lsn_   = 0;
    uint32_t    appends_since_ckpt_ = 0;

    static constexpr size_t   HEADER_SIZE   = 64;
    static constexpr uint32_t MAGIC         = 0x57414C21; // "WAL!"
    static constexpr uint32_t VERSION       = 1;
    static constexpr size_t   INITIAL_SIZE  = 4ULL * 1024 * 1024; // 4 MB
    static constexpr size_t   GROW_SIZE     = 16ULL * 1024 * 1024; // grow by 16 MB
};

} // namespace vecdb