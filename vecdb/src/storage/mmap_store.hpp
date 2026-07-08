#pragma once

// ──────────────────────────────────────────────────────────────────────────────
//  MmapStore — Phase 4
//
//  Zero-copy persistent storage for the raw float vectors.
//
//  Design:
//    - A single flat binary file: 64-byte header + raw float32 data
//    - The float array is mmap()'d directly — no serialisation, no memcpy
//    - Save  = msync()  (flush dirty pages to disk) — O(1) CPU time
//    - Load  = mmap()   (map file into address space) — O(1) CPU time
//    - The OS page cache acts as an automatic LRU; hot vectors stay in RAM,
//      cold ones are evicted transparently
//
//  Segment file binary layout:
//    Offset  Size  Field
//    ──────  ────  ──────────────────────────────────────────────────
//      0      4    magic        0x56454344 ("VECD")
//      4      2    version      format version (currently 1)
//      6      2    flags        reserved, must be 0
//      8      8    vec_count    number of vectors stored (uint64_t)
//     16      4    dim          floats per vector (uint32_t)
//     20      4    header_crc   CRC32 of bytes 0..19
//     24      8    created_ns   creation timestamp (Unix nanoseconds)
//     32     32    reserved     zero-padded
//     64      …    float data   vec_count * dim * 4 bytes, row-major
//
//  Thread safety:
//    - Concurrent reads are safe (shared OS mapping)
//    - Writes (append) must be called with the index write lock held
// ──────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstddef>
#include <string>

namespace vecdb {

class MmapStore {
public:
    // Open an existing segment file for reading.
    // Throws if the file doesn't exist or has an invalid header.
    static MmapStore open(const std::string& path);

    // Create a new empty segment file for `dim`-dimensional vectors.
    // Throws if the file already exists.
    static MmapStore create(const std::string& path, uint32_t dim);

    ~MmapStore();

    // Move-only (owns the mmap region)
    MmapStore(MmapStore&&) noexcept;
    MmapStore& operator=(MmapStore&&) noexcept;
    MmapStore(const MmapStore&)            = delete;
    MmapStore& operator=(const MmapStore&) = delete;

    // ── Write path ────────────────────────────────────────────────────────────

    // Append one vector to the store.
    // `vec` must point to `dim_` floats.
    // Grows the file if needed.
    // Returns the node_id (== index of this vector in the store).
    uint32_t append(const float* vec);

    // Flush dirty pages to disk synchronously.
    // Call after a batch of appends or before taking a snapshot.
    void sync();

    // ── Read path ─────────────────────────────────────────────────────────────

    // Return a pointer directly into the mmap region.
    // Valid until the next append() that triggers a remap.
    // Caller must NOT write through this pointer.
    const float* get(uint32_t node_id) const;

    // ── Introspection ─────────────────────────────────────────────────────────
    uint32_t vec_count() const { return vec_count_; }
    uint32_t dim()       const { return dim_; }
    size_t   file_bytes() const { return map_size_; }

    const std::string& path() const { return path_; }

private:
    MmapStore() = default;

    void write_header();
    void read_and_validate_header();
    void grow(size_t new_size);

    static uint32_t crc32_header(const uint8_t* h);

    std::string path_;
    int         fd_        = -1;
    uint8_t*    map_       = nullptr;
    size_t      map_size_  = 0;

    uint32_t    dim_       = 0;
    uint32_t    vec_count_ = 0;

    static constexpr size_t   HEADER_SIZE  = 64;
    static constexpr uint32_t MAGIC        = 0x56454344; // "VECD"
    static constexpr uint16_t VERSION      = 1;
    static constexpr size_t   INITIAL_DATA = 1024ULL * 1024;      // 1 MB initial
    static constexpr size_t   GROW_BYTES   = 64ULL * 1024 * 1024; // grow 64 MB
};

} // namespace vecdb