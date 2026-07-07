#include "wal.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace vecdb {

// ──────────────────────────────────────────────────────────────────────────────
//  CRC32 — fast lookup-table implementation
//  Used to detect torn writes on replay.
// ──────────────────────────────────────────────────────────────────────────────
static uint32_t crc32_table[256];
static bool     crc32_table_ready = false;

static void build_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = true;
}

static uint32_t crc32(const void* data, size_t len) {
    if (!crc32_table_ready) build_crc32_table();
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ──────────────────────────────────────────────────────────────────────────────
//  Entry layout helpers
//
//  Each entry on disk:
//    [8] LSN  [8] vector_id  [4] dim  [4] CRC32  [dim*4] floats
// ──────────────────────────────────────────────────────────────────────────────
static constexpr size_t LSN_OFF    = 0;
static constexpr size_t VID_OFF    = 8;
static constexpr size_t DIM_OFF    = 16;
static constexpr size_t CRC_OFF    = 20;
static constexpr size_t DATA_OFF   = 24;
static constexpr size_t FIXED_HDR  = 24; // bytes before the float data

// ──────────────────────────────────────────────────────────────────────────────
//  Constructor
// ──────────────────────────────────────────────────────────────────────────────
WAL::WAL(const std::string& path, uint32_t dim, uint32_t checkpoint_every)
    : path_(path)
    , dim_(dim)
    , checkpoint_every_(checkpoint_every)
{
    if (dim == 0) throw std::invalid_argument("WAL: dim must be > 0");
    open_or_create();
}

// ──────────────────────────────────────────────────────────────────────────────
//  Destructor — sync and unmap
// ──────────────────────────────────────────────────────────────────────────────
WAL::~WAL() {
    if (map_ && map_ != MAP_FAILED) {
        msync(map_, map_size_, MS_SYNC);
        munmap(map_, map_size_);
    }
    if (fd_ >= 0) close(fd_);
}

// ──────────────────────────────────────────────────────────────────────────────
//  entry_size — bytes per log entry
// ──────────────────────────────────────────────────────────────────────────────
size_t WAL::entry_size() const {
    return FIXED_HDR + static_cast<size_t>(dim_) * sizeof(float);
}

// ──────────────────────────────────────────────────────────────────────────────
//  entry_ptr — pointer to the start of entry with given lsn in the mmap region
// ──────────────────────────────────────────────────────────────────────────────
uint8_t* WAL::entry_ptr(uint64_t lsn) const {
    return map_ + HEADER_SIZE + lsn * entry_size();
}

// ──────────────────────────────────────────────────────────────────────────────
//  file_size_bytes
// ──────────────────────────────────────────────────────────────────────────────
size_t WAL::file_size_bytes() const {
    return map_size_;
}

// ──────────────────────────────────────────────────────────────────────────────
//  crc32_vec — checksum of the float payload of one entry
// ──────────────────────────────────────────────────────────────────────────────
uint32_t WAL::crc32_vec(const float* vec, uint32_t dim) const {
    return crc32(vec, static_cast<size_t>(dim) * sizeof(float));
}

// ──────────────────────────────────────────────────────────────────────────────
//  write_header — write the 64-byte file header into the mmap region
// ──────────────────────────────────────────────────────────────────────────────
void WAL::write_header() {
    uint8_t* h = map_;
    std::memset(h, 0, HEADER_SIZE);

    uint32_t magic   = MAGIC;
    uint32_t version = VERSION;
    std::memcpy(h + 0,  &magic,           4);
    std::memcpy(h + 4,  &version,         4);
    std::memcpy(h + 8,  &checkpoint_lsn_, 8);
    std::memcpy(h + 16, &dim_,            4);

    // Header CRC covers bytes 0..19
    uint32_t hcrc = crc32(h, 20);
    std::memcpy(h + 20, &hcrc, 4);
}

// ──────────────────────────────────────────────────────────────────────────────
//  read_header — read checkpoint_lsn and dim from an existing file
// ──────────────────────────────────────────────────────────────────────────────
void WAL::read_header() {
    uint8_t* h = map_;

    uint32_t magic, version, stored_dim, hcrc;
    uint64_t ckpt_lsn;

    std::memcpy(&magic,      h + 0,  4);
    std::memcpy(&version,    h + 4,  4);
    std::memcpy(&ckpt_lsn,  h + 8,  8);
    std::memcpy(&stored_dim, h + 16, 4);
    std::memcpy(&hcrc,       h + 20, 4);

    if (magic != MAGIC)
        throw std::runtime_error("WAL: bad magic — file is not a vecdb WAL");
    if (version != VERSION)
        throw std::runtime_error("WAL: unsupported WAL version");

    uint32_t expected_crc = crc32(h, 20);
    if (hcrc != expected_crc)
        throw std::runtime_error("WAL: header CRC mismatch — file may be corrupt");

    if (stored_dim != 0 && stored_dim != dim_)
        throw std::runtime_error("WAL: dim mismatch — file was created with different dim");

    checkpoint_lsn_ = ckpt_lsn;
    next_lsn_       = 0; // will be advanced during scan below
}

// ──────────────────────────────────────────────────────────────────────────────
//  grow — extend the file and remap
// ──────────────────────────────────────────────────────────────────────────────
void WAL::grow(size_t new_size) {
    // Unmap current region
    if (map_ && map_ != MAP_FAILED) {
        msync(map_, map_size_, MS_ASYNC);
        munmap(map_, map_size_);
        map_ = nullptr;
    }

    // Extend the file
    if (ftruncate(fd_, static_cast<off_t>(new_size)) != 0)
        throw std::runtime_error("WAL: ftruncate failed");

    // Remap
    void* p = mmap(nullptr, new_size,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED)
        throw std::runtime_error("WAL: mmap failed after grow");

    map_      = static_cast<uint8_t*>(p);
    map_size_ = new_size;
}

// ──────────────────────────────────────────────────────────────────────────────
//  open_or_create — open the file, mmap it, read or write the header
// ──────────────────────────────────────────────────────────────────────────────
void WAL::open_or_create() {
    bool is_new = (access(path_.c_str(), F_OK) != 0);

    fd_ = open(path_.c_str(),
               O_RDWR | O_CREAT,
               S_IRUSR | S_IWUSR);
    if (fd_ < 0)
        throw std::runtime_error("WAL: cannot open file: " + path_);

    if (is_new) {
        // Brand new file — allocate initial space
        if (ftruncate(fd_, static_cast<off_t>(INITIAL_SIZE)) != 0)
            throw std::runtime_error("WAL: ftruncate failed on new file");

        void* p = mmap(nullptr, INITIAL_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED)
            throw std::runtime_error("WAL: mmap failed on new file");

        map_      = static_cast<uint8_t*>(p);
        map_size_ = INITIAL_SIZE;

        write_header();
        msync(map_, HEADER_SIZE, MS_SYNC);
    } else {
        // Existing file — map it fully
        struct stat st{};
        fstat(fd_, &st);
        size_t file_sz = static_cast<size_t>(st.st_size);
        if (file_sz < HEADER_SIZE)
            throw std::runtime_error("WAL: file too small to contain a valid header");

        // Ensure mapped size is at least INITIAL_SIZE
        size_t map_sz = std::max(file_sz, INITIAL_SIZE);
        if (map_sz > file_sz) {
            if (ftruncate(fd_, static_cast<off_t>(map_sz)) != 0)
                throw std::runtime_error("WAL: ftruncate failed on existing file");
        }

        void* p = mmap(nullptr, map_sz,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED)
            throw std::runtime_error("WAL: mmap failed on existing file");

        map_      = static_cast<uint8_t*>(p);
        map_size_ = map_sz;

        read_header();
        // Scan from 0 to count all valid entries and set next_lsn_
        uint64_t lsn = 0;
        while (true) {
            uint8_t* ep = entry_ptr(lsn);
            if (ep + entry_size() > map_ + map_size_) break;
            // Check if entry looks valid — LSN field matches
            uint64_t stored_lsn;
            std::memcpy(&stored_lsn, ep + LSN_OFF, 8);
            if (stored_lsn != lsn) break;
            // Verify CRC
            uint32_t stored_crc;
            uint32_t stored_dim;
            std::memcpy(&stored_crc, ep + CRC_OFF, 4);
            std::memcpy(&stored_dim, ep + DIM_OFF, 4);
            if (stored_dim != dim_) break;
            uint32_t computed_crc = crc32(ep + DATA_OFF,
                                          static_cast<size_t>(dim_) * sizeof(float));
            if (computed_crc != stored_crc) break;
            ++lsn;
        }
        next_lsn_ = lsn;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  append — write one WAL entry
// ──────────────────────────────────────────────────────────────────────────────
uint64_t WAL::append(uint64_t vector_id, const float* vec) {
    // Ensure enough space
    size_t needed = HEADER_SIZE + (next_lsn_ + 1) * entry_size();
    if (needed > map_size_) {
        grow(map_size_ + GROW_SIZE);
    }

    uint8_t* ep = entry_ptr(next_lsn_);

    // Write fields
    uint64_t lsn = next_lsn_;
    uint32_t d   = dim_;
    uint32_t checksum = crc32_vec(vec, dim_);

    std::memcpy(ep + LSN_OFF,  &lsn,       8);
    std::memcpy(ep + VID_OFF,  &vector_id, 8);
    std::memcpy(ep + DIM_OFF,  &d,         4);
    std::memcpy(ep + CRC_OFF,  &checksum,  4);
    std::memcpy(ep + DATA_OFF, vec, static_cast<size_t>(dim_) * sizeof(float));

    ++next_lsn_;
    ++appends_since_ckpt_;

    // Auto checkpoint if threshold reached
    if (checkpoint_every_ > 0 && appends_since_ckpt_ >= checkpoint_every_) {
        checkpoint();
    }

    return lsn;
}

// ──────────────────────────────────────────────────────────────────────────────
//  checkpoint — flush to disk and record the safe LSN in the header
// ──────────────────────────────────────────────────────────────────────────────
void WAL::checkpoint() {
    checkpoint_lsn_     = next_lsn_;
    appends_since_ckpt_ = 0;

    // Update checkpoint_lsn in header
    std::memcpy(map_ + 8, &checkpoint_lsn_, 8);

    // Recompute header CRC
    uint32_t hcrc = crc32(map_, 20);
    std::memcpy(map_ + 20, &hcrc, 4);

    // Synchronous flush — guarantees data is on disk
    msync(map_, map_size_, MS_SYNC);
}

// ──────────────────────────────────────────────────────────────────────────────
//  replay — scan forward from checkpoint_lsn_, call fn for each valid entry
// ──────────────────────────────────────────────────────────────────────────────
uint64_t WAL::replay(std::function<void(const WALEntry&)> fn) const {
    uint64_t count = 0;
    uint64_t lsn   = 0; // always replay from beginning of file

    while (true) {
        uint8_t* ep = entry_ptr(lsn);

        // Bounds check — don't read past the mapped region
        if (ep + entry_size() > map_ + map_size_) break;

        // Read and validate LSN field
        uint64_t stored_lsn;
        std::memcpy(&stored_lsn, ep + LSN_OFF, 8);
        if (stored_lsn != lsn) break; // missing or out-of-order entry

        // Read dim — must match
        uint32_t stored_dim;
        std::memcpy(&stored_dim, ep + DIM_OFF, 4);
        if (stored_dim != dim_) break;

        // CRC check — detect torn writes
        uint32_t stored_crc;
        std::memcpy(&stored_crc, ep + CRC_OFF, 4);
        uint32_t computed_crc = crc32(ep + DATA_OFF,
                                       static_cast<size_t>(dim_) * sizeof(float));
        if (stored_crc != computed_crc) {
            // Torn write — stop here, this entry is incomplete
            break;
        }

        // Valid entry — build WALEntry and call the user callback
        uint64_t vid;
        std::memcpy(&vid, ep + VID_OFF, 8);

        WALEntry entry{};
        entry.lsn       = lsn;
        entry.vector_id = vid;
        entry.dim       = stored_dim;
        entry.vec       = reinterpret_cast<const float*>(ep + DATA_OFF);

        fn(entry);
        ++count;
        ++lsn;
    }

    return count;
}

// ──────────────────────────────────────────────────────────────────────────────
//  truncate — discard all entries before keep_from_lsn
//  Used after full snapshot to reclaim disk space.
// ──────────────────────────────────────────────────────────────────────────────
void WAL::truncate(uint64_t keep_from_lsn) {
    if (keep_from_lsn <= checkpoint_lsn_) return;
    if (keep_from_lsn >= next_lsn_) keep_from_lsn = next_lsn_;

    // Compute bytes to keep
    size_t entries_to_keep = next_lsn_ - keep_from_lsn;
    size_t data_to_keep    = entries_to_keep * entry_size();
    size_t src_offset      = HEADER_SIZE + keep_from_lsn * entry_size();
    size_t new_file_size   = HEADER_SIZE + data_to_keep;

    // Shift data left in the mmap region
    if (data_to_keep > 0 && src_offset < map_size_) {
        std::memmove(map_ + HEADER_SIZE,
                     map_ + src_offset,
                     data_to_keep);
    }

    // Update LSN fields in shifted entries to be 0-based again
    uint64_t new_lsn = 0;
    for (size_t i = 0; i < entries_to_keep; ++i) {
        uint8_t* ep = map_ + HEADER_SIZE + i * entry_size();
        std::memcpy(ep + LSN_OFF, &new_lsn, 8);
        ++new_lsn;
    }

    // Reset internal state
    next_lsn_         = new_lsn;
    checkpoint_lsn_   = 0;
    appends_since_ckpt_ = 0;

    // Truncate the file
    if (map_ && map_ != MAP_FAILED) {
        msync(map_, map_size_, MS_SYNC);
        munmap(map_, map_size_);
        map_ = nullptr;
    }
    if (ftruncate(fd_, static_cast<off_t>(new_file_size)) != 0)
        throw std::runtime_error("WAL: ftruncate failed during truncate");

    // Remap at new size (grow to at least INITIAL_SIZE)
    size_t remap_size = std::max(new_file_size, INITIAL_SIZE);
    if (remap_size > new_file_size) {
        if (ftruncate(fd_, static_cast<off_t>(remap_size)) != 0)
            throw std::runtime_error("WAL: ftruncate failed during remap");
    }
    void* p = mmap(nullptr, remap_size,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED)
        throw std::runtime_error("WAL: mmap failed after truncate");
    map_      = static_cast<uint8_t*>(p);
    map_size_ = remap_size;

    write_header();
    msync(map_, HEADER_SIZE, MS_SYNC);
}

} // namespace vecdb