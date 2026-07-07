// Phase 3 — WAL unit tests
//
// Tests:
//   1. Basic append and replay
//   2. CRC catches torn writes (truncated entry)
//   3. Checkpoint LSN persists across reopen
//   4. Replay stops at invalid CRC
//   5. Auto-checkpoint threshold
//   6. Truncate reclaims entries

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "src/wal/wal.hpp"

#include <cstring>
#include <cstdio>
#include <vector>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ── Helper: temp file path that auto-deletes ──────────────────────────────────
struct TempFile {
    std::string path;
    explicit TempFile(const std::string& name) {
        path = "/tmp/vecdb_test_" + name + ".wal";
        std::remove(path.c_str());
    }
    ~TempFile() { std::remove(path.c_str()); }
};

static std::vector<float> make_vec(int dim, float fill) {
    return std::vector<float>(dim, fill);
}

// ── Test 1: append and full replay ───────────────────────────────────────────

TEST_CASE("WAL: append and replay all entries") {
    TempFile tf("append_replay");
    const int DIM = 8;
    const int N   = 100;

    {
        vecdb::WAL wal(tf.path, DIM, 0); // checkpoint_every=0: manual only
        for (int i = 0; i < N; ++i) {
            auto v = make_vec(DIM, static_cast<float>(i));
            wal.append(static_cast<uint64_t>(i), v.data());
        }
        wal.checkpoint();
    }

    // Reopen and replay
    vecdb::WAL wal2(tf.path, DIM, 0);
    int count = 0;
    wal2.replay([&](const vecdb::WALEntry& e) {
        CHECK(e.lsn       == static_cast<uint64_t>(count));
        CHECK(e.vector_id == static_cast<uint64_t>(count));
        CHECK(e.dim       == static_cast<uint32_t>(DIM));
        // First float should equal the index
        CHECK(e.vec[0] == doctest::Approx(static_cast<float>(count)).epsilon(1e-6f));
        ++count;
    });
    CHECK(count == N);
}

// ── Test 2: torn write detection ─────────────────────────────────────────────

TEST_CASE("WAL: replay stops at torn write") {
    TempFile tf("torn_write");
    const int DIM = 4;

    {
        vecdb::WAL wal(tf.path, DIM, 0);
        for (int i = 0; i < 10; ++i) {
            auto v = make_vec(DIM, static_cast<float>(i));
            wal.append(static_cast<uint64_t>(i), v.data());
        }
        wal.checkpoint(); // checkpoint at lsn=10
        // Write 5 more entries (not checkpointed)
        for (int i = 10; i < 15; ++i) {
            auto v = make_vec(DIM, static_cast<float>(i));
            wal.append(static_cast<uint64_t>(i), v.data());
        }
        // Simulate torn write: corrupt the CRC of entry 12
        // We do this by directly modifying the file AFTER wal goes out of scope
    }

    // Corrupt entry at LSN=12: flip a byte in the float data
    {
        std::fstream f(tf.path, std::ios::in | std::ios::out | std::ios::binary);
        // Entry layout: HEADER(64) + lsn * entry_size
        // entry_size = 24 + dim*4 = 24 + 16 = 40
        const size_t entry_sz = 24 + DIM * 4;
        const size_t offset   = 64 + 12 * entry_sz + 24; // DATA_OFF of entry 12
        f.seekp(static_cast<std::streamoff>(offset));
        uint8_t bad = 0xFF;
        f.write(reinterpret_cast<char*>(&bad), 1);
    }

    // Replay always starts from LSN=0. Entries 0..11 are good (12 valid),
    // entry 12 has corrupt CRC → replay stops there.
    vecdb::WAL wal2(tf.path, DIM, 0);
    int count = 0;
    wal2.replay([&](const vecdb::WALEntry& e) {
        (void)e;
        ++count;
    });
    CHECK(count == 12);
}

// ── Test 3: checkpoint LSN persists across reopen ─────────────────────────────

TEST_CASE("WAL: checkpoint LSN survives reopen") {
    TempFile tf("checkpoint_persist");
    const int DIM = 4;

    {
        vecdb::WAL wal(tf.path, DIM, 0);
        for (int i = 0; i < 50; ++i) {
            auto v = make_vec(DIM, 1.0f);
            wal.append(static_cast<uint64_t>(i), v.data());
        }
        wal.checkpoint(); // checkpoint_lsn = 50
    }

    vecdb::WAL wal2(tf.path, DIM, 0);
    CHECK(wal2.checkpoint_lsn() == 50u);
    CHECK(wal2.current_lsn()    == 50u);
}

// ── Test 4: auto-checkpoint every N appends ───────────────────────────────────

TEST_CASE("WAL: auto-checkpoint fires at threshold") {
    TempFile tf("auto_ckpt");
    const int DIM   = 4;
    const int EVERY = 10;

    vecdb::WAL wal(tf.path, DIM, EVERY);
    for (int i = 0; i < 25; ++i) {
        auto v = make_vec(DIM, 1.0f);
        wal.append(static_cast<uint64_t>(i), v.data());
    }
    // After 25 appends with checkpoint_every=10:
    // auto-checkpoint fires at 10 and 20 → checkpoint_lsn = 20
    CHECK(wal.checkpoint_lsn() == 20u);
    CHECK(wal.current_lsn()    == 25u);
}

// ── Test 5: replay after crash (no explicit checkpoint) ──────────────────────

TEST_CASE("WAL: replay recovers entries written after last checkpoint") {
    TempFile tf("crash_recovery");
    const int DIM = 8;

    // Simulate: checkpoint at 5, then 5 more writes, then "crash" (no final ckpt)
    {
        vecdb::WAL wal(tf.path, DIM, 0);
        for (int i = 0; i < 5; ++i) {
            auto v = make_vec(DIM, static_cast<float>(i));
            wal.append(static_cast<uint64_t>(i), v.data());
        }
        wal.checkpoint(); // lsn=5 checkpointed
        for (int i = 5; i < 10; ++i) {
            auto v = make_vec(DIM, static_cast<float>(i));
            wal.append(static_cast<uint64_t>(i), v.data());
        }
        // "crash" — destructor still calls msync so data is on disk
    }

    // On restart, replay from checkpoint_lsn=5; should get entries 5..9
    vecdb::WAL wal2(tf.path, DIM, 0);
    CHECK(wal2.checkpoint_lsn() == 5u);

    // replay() starts from LSN=0, replays all 10 valid entries (5 ckpt + 5 extra)
    int count = 0;
    wal2.replay([&](const vecdb::WALEntry& e) {
        (void)e;
        ++count;
    });
    CHECK(count == 10);
}

// ── Test 6: file grows beyond initial size ────────────────────────────────────

TEST_CASE("WAL: file grows automatically for large payloads") {
    TempFile tf("grow");
    const int DIM = 1536; // large embedding size
    const int N   = 5000; // enough to exceed the 4MB initial size

    vecdb::WAL wal(tf.path, DIM, 0);
    for (int i = 0; i < N; ++i) {
        auto v = make_vec(DIM, static_cast<float>(i % 100));
        wal.append(static_cast<uint64_t>(i), v.data());
    }

    CHECK(wal.current_lsn()    == static_cast<uint64_t>(N));
    CHECK(wal.file_size_bytes() > 4ULL * 1024 * 1024); // grew beyond initial 4MB

    wal.checkpoint();

    // Verify all entries survive
    int count = 0;
    wal.replay([&](const vecdb::WALEntry& e) { (void)e; ++count; });
    CHECK(count == N);
}