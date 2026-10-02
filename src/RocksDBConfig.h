#pragma once
//
// RocksDBConfig.h — the SINGLE source of RocksDB options.
//
// Both the standalone RocksDB baseline and AHSE's internal RocksDB instance
// construct their options here. This makes their configuration provably
// identical and directly answers Reviewer #2's D1 / hypothesis H2 ("state
// whether the two RocksDB instances use exactly the same version, options,
// compaction settings, cache settings").
//
// The configuration is deliberately close to RocksDB defaults; every non-default
// choice is explicit and commented so it can be reported in the paper's setup.
//
#include <cstdint>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/version.h>
#include <string>
#include <memory>

namespace ahse {

// Diagnostic: snapshot RocksDB background-work counters, for correlating
// per-rep latency spikes with in-flight/pending compaction (Reviewer question
// on the C@10M variance). Cheap; called only at phase boundaries, never on the
// hot path.
inline std::string rocks_compaction_state(rocksdb::DB* db) {
    if (!db) return "";
    auto p = [&](const char* name) -> std::string {
        std::string v = "?";
        db->GetProperty(name, &v);
        return v;
    };
    return "cpend=" + p("rocksdb.compaction-pending") +
           " nrun_comp=" + p("rocksdb.num-running-compactions") +
           " nrun_flush=" + p("rocksdb.num-running-flushes") +
           " memtable_flush_pending=" + p("rocksdb.mem-table-flush-pending") +
           " pend_comp_bytes=" + p("rocksdb.estimate-pending-compaction-bytes") +
           " l0files=" + p("rocksdb.num-files-at-level0");
}

// Warm-up: sweep every SSTable once so the block/OS page cache is primed for
// the timed workload. Touching one byte per 512 forces each page resident.
// Reads go straight through the RocksDB iterator (no engine adaptive path).
inline void rocks_prewarm(rocksdb::DB* db) {
    if (!db) return;
    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions()));
    volatile uint64_t sink = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        rocksdb::Slice v = it->value();
        for (size_t i = 0; i < v.size(); i += 512) sink += (unsigned char)v.data()[i];
    }
    (void)sink;
}

// Portable DB open. RocksDB changed the single-Options Open() signature across
// versions: newer builds (>= 8, incl. the Homebrew 11.x used on macOS) expose
// Open(Options, name, std::unique_ptr<DB>*), while older ones (e.g. the 6.x that
// ships with Ubuntu 22.04 on WSL) expose the classic Open(Options, name, DB**).
// Branch on ROCKSDB_MAJOR so the same source builds on either. (DB-open plumbing
// only — no effect on read/write semantics.)
inline rocksdb::Options rocksdb_options();  // fwd decl
inline rocksdb::Status open_rocksdb(const std::string& path,
                                    std::unique_ptr<rocksdb::DB>& out) {
#if defined(ROCKSDB_MAJOR) && (ROCKSDB_MAJOR >= 8)
    std::unique_ptr<rocksdb::DB> up;
    rocksdb::Status s = rocksdb::DB::Open(rocksdb_options(), path, &up);
    if (s.ok()) out = std::move(up);
    return s;
#else
    rocksdb::DB* raw = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(rocksdb_options(), path, &raw);
    if (s.ok()) out.reset(raw);
    return s;
#endif
}

inline rocksdb::Options rocksdb_options() {
    rocksdb::Options options;
    options.create_if_missing = true;

    // Explicit, documented, and identical for baseline and AHSE-internal RocksDB.
    // Left at RocksDB defaults intentionally (no custom block cache, write buffer,
    // bloom filter, or compaction tuning) so the comparison isolates the
    // architectural difference (LSM vs adaptive B+-tree routing) rather than
    // config tuning. If tuning is added later, it MUST stay in this one place.
    options.compression = rocksdb::kSnappyCompression; // RocksDB default on this build

    return options;
}

} // namespace ahse
