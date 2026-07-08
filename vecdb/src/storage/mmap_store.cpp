#include "mmap_store.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <stdexcept>
#include <algorithm>

namespace vecdb {

// ── CRC32 (same table-based impl as WAL) ──────────────────────────────────────
static uint32_t s_crc_table[256];
static bool     s_crc_ready = false;

static void init_crc() {
    if (s_crc_ready) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

static uint32_t crc32_buf(const void* data, size_t len) {
    init_crc();
    uint32_t crc = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
        crc = s_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ── Header field offsets ──────────────────────────────────────────────────────
static constexpr size_t OFF_MAGIC      = 0;
static constexpr size_t OFF_VERSION    = 4;
static constexpr size_t OFF_FLAGS      = 6;
static constexpr size_t OFF_VEC_COUNT  = 8;
static constexpr size_t OFF_DIM        = 16;
static constexpr size_t OFF_HDR_CRC    = 20;
static constexpr size_t OFF_CREATED_NS = 24;
// bytes 32..63 reserved

// ── Helpers ───────────────────────────────────────────────────────────────────

uint32_t MmapStore::crc32_header(const uint8_t* h) {
    // CRC covers the first 20 bytes (everything before the CRC field itself)
    return crc32_buf(h, 20);
}

static size_t data_offset() { return 64; }

static size_t required_size(uint32_t vec_count, uint32_t dim) {
    return 64
         + static_cast<size_t>(vec_count) * dim * sizeof(float);
}

// ── Factory: create ───────────────────────────────────────────────────────────

MmapStore MmapStore::create(const std::string& path, uint32_t dim) {
    if (dim == 0) throw std::invalid_argument("MmapStore: dim must be > 0");

    MmapStore s;
    s.path_      = path;
    s.dim_       = dim;
    s.vec_count_ = 0;

    s.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (s.fd_ < 0)
        throw std::runtime_error("MmapStore: cannot create file: " + path);

    // Initial file size: header + 1 MB of data space
    size_t init_size = HEADER_SIZE + INITIAL_DATA;
    if (ftruncate(s.fd_, static_cast<off_t>(init_size)) != 0)
        throw std::runtime_error("MmapStore: ftruncate failed on create");

    void* p = mmap(nullptr, init_size, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd_, 0);
    if (p == MAP_FAILED)
        throw std::runtime_error("MmapStore: mmap failed on create");

    s.map_      = static_cast<uint8_t*>(p);
    s.map_size_ = init_size;

    s.write_header();
    msync(s.map_, HEADER_SIZE, MS_SYNC);
    return s;
}

// ── Factory: open ─────────────────────────────────────────────────────────────

MmapStore MmapStore::open(const std::string& path) {
    MmapStore s;
    s.path_ = path;

    s.fd_ = ::open(path.c_str(), O_RDWR, S_IRUSR | S_IWUSR);
    if (s.fd_ < 0)
        throw std::runtime_error("MmapStore: cannot open file: " + path);

    struct stat st{};
    fstat(s.fd_, &st);
    size_t file_sz = static_cast<size_t>(st.st_size);

    if (file_sz < HEADER_SIZE)
        throw std::runtime_error("MmapStore: file too small");

    void* p = mmap(nullptr, file_sz, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd_, 0);
    if (p == MAP_FAILED)
        throw std::runtime_error("MmapStore: mmap failed on open");

    s.map_      = static_cast<uint8_t*>(p);
    s.map_size_ = file_sz;

    s.read_and_validate_header();
    return s;
}

// ── Destructor ────────────────────────────────────────────────────────────────

MmapStore::~MmapStore() {
    if (map_ && map_ != MAP_FAILED) {
        msync(map_, map_size_, MS_ASYNC);
        munmap(map_, map_size_);
    }
    if (fd_ >= 0) close(fd_);
}

// ── Move semantics ────────────────────────────────────────────────────────────

MmapStore::MmapStore(MmapStore&& o) noexcept
    : path_(std::move(o.path_))
    , fd_(o.fd_), map_(o.map_), map_size_(o.map_size_)
    , dim_(o.dim_), vec_count_(o.vec_count_)
{
    o.fd_ = -1; o.map_ = nullptr; o.map_size_ = 0;
}

MmapStore& MmapStore::operator=(MmapStore&& o) noexcept {
    if (this == &o) return *this;
    this->~MmapStore();
    new (this) MmapStore(std::move(o));
    return *this;
}

// ── write_header ─────────────────────────────────────────────────────────────

void MmapStore::write_header() {
    uint8_t* h = map_;
    std::memset(h, 0, HEADER_SIZE);

    uint32_t magic   = MAGIC;
    uint16_t version = VERSION;
    uint16_t flags   = 0;
    uint64_t vc      = vec_count_;
    uint32_t d       = dim_;

    std::memcpy(h + OFF_MAGIC,     &magic,   4);
    std::memcpy(h + OFF_VERSION,   &version, 2);
    std::memcpy(h + OFF_FLAGS,     &flags,   2);
    std::memcpy(h + OFF_VEC_COUNT, &vc,      8);
    std::memcpy(h + OFF_DIM,       &d,       4);

    uint32_t crc = crc32_header(h);
    std::memcpy(h + OFF_HDR_CRC, &crc, 4);

    // Timestamp (nanoseconds since epoch)
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                + static_cast<uint64_t>(ts.tv_nsec);
    std::memcpy(h + OFF_CREATED_NS, &ns, 8);
}

// ── read_and_validate_header ──────────────────────────────────────────────────

void MmapStore::read_and_validate_header() {
    uint8_t* h = map_;

    uint32_t magic, d, stored_crc;
    uint16_t version;
    uint64_t vc;

    std::memcpy(&magic,       h + OFF_MAGIC,     4);
    std::memcpy(&version,     h + OFF_VERSION,   2);
    std::memcpy(&vc,          h + OFF_VEC_COUNT, 8);
    std::memcpy(&d,           h + OFF_DIM,       4);
    std::memcpy(&stored_crc,  h + OFF_HDR_CRC,   4);

    if (magic != MAGIC)
        throw std::runtime_error("MmapStore: bad magic — not a vecdb segment file");
    if (version != VERSION)
        throw std::runtime_error("MmapStore: unsupported segment version");

    uint32_t expected_crc = crc32_header(h);
    if (stored_crc != expected_crc)
        throw std::runtime_error("MmapStore: header CRC mismatch");

    dim_       = d;
    vec_count_ = static_cast<uint32_t>(vc);
}

// ── grow ─────────────────────────────────────────────────────────────────────

void MmapStore::grow(size_t new_size) {
    if (map_ && map_ != MAP_FAILED) {
        msync(map_, map_size_, MS_ASYNC);
        munmap(map_, map_size_);
        map_ = nullptr;
    }
    if (ftruncate(fd_, static_cast<off_t>(new_size)) != 0)
        throw std::runtime_error("MmapStore: ftruncate failed during grow");

    void* p = mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED)
        throw std::runtime_error("MmapStore: mmap failed after grow");

    map_      = static_cast<uint8_t*>(p);
    map_size_ = new_size;
}

// ── append ────────────────────────────────────────────────────────────────────

uint32_t MmapStore::append(const float* vec) {
    // Ensure space for the new vector
    size_t needed = required_size(vec_count_ + 1, dim_);
    if (needed > map_size_) {
        grow(map_size_ + GROW_BYTES);
    }

    // Write directly into the mmap region
    float* dst = reinterpret_cast<float*>(map_ + data_offset())
               + static_cast<size_t>(vec_count_) * dim_;
    std::memcpy(dst, vec, static_cast<size_t>(dim_) * sizeof(float));

    uint32_t id = vec_count_++;

    // Update vec_count in header (no full header rewrite — just patch the field)
    uint64_t vc = vec_count_;
    std::memcpy(map_ + OFF_VEC_COUNT, &vc, 8);

    // Patch CRC
    uint32_t crc = crc32_header(map_);
    std::memcpy(map_ + OFF_HDR_CRC, &crc, 4);

    return id;
}

// ── get ───────────────────────────────────────────────────────────────────────

const float* MmapStore::get(uint32_t node_id) const {
    if (node_id >= vec_count_)
        throw std::out_of_range("MmapStore::get: node_id out of range");
    return reinterpret_cast<const float*>(map_ + data_offset())
         + static_cast<size_t>(node_id) * dim_;
}

// ── sync ──────────────────────────────────────────────────────────────────────

void MmapStore::sync() {
    if (map_ && map_ != MAP_FAILED)
        msync(map_, map_size_, MS_SYNC);
}

} // namespace vecdb