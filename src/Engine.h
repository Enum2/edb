#pragma once
//
// Engine.h — the single storage-engine interface every backend implements.
//
// This is the abstraction the reviewers implicitly asked for: it lets the
// harness drive RocksDB, LMDB, AHSE, AHA, and StaticHybrid through *identical*
// call paths so the comparison is apples-to-apples.
//
// Read semantics are split explicitly:
//   * get()  : point lookup  (YCSB READ)
//   * scan() : range scan    (YCSB SCAN)
//   * put()  : insert/update  (YCSB UPDATE / INSERT)
//
// Both get() and scan() MUST materialize the values they touch (fold them into
// checksum_) so the optimizer cannot elide the work — this fixes the old
// "measured scans do not seem to materialize" defect (Reviewer #3, O4).
//
#include <string>
#include <cstdint>

class Engine {
public:
    virtual ~Engine() = default;

    // Human-readable engine name used in telemetry / result tables.
    virtual std::string name() const = 0;

    // Insert or update a key. INSERT and UPDATE are the same operation here.
    virtual void put(const std::string& key, const std::string& value) = 0;

    // Point lookup. Returns true if found; the value is materialized internally.
    virtual bool get(const std::string& key) = 0;

    // Inclusive range scan [start_key, end_key]. Returns the number of records
    // visited. Every value touched is materialized.
    virtual uint64_t scan(const std::string& start_key, const std::string& end_key) = 0;

    // Optional: called once after the load phase, before timed operations.
    // StaticHybrid uses this to build its LMDB index during warmup.
    virtual void on_load_complete() {}

    // Optional diagnostic: a snapshot of engine-internal state (e.g. RocksDB
    // compaction counters) for correlating latency spikes with background work.
    // Default empty; RocksDB-backed engines override it.
    virtual std::string debug_state() { return ""; }

    // Optional warm-up: prime the OS page / block cache for the data the timed
    // workload will read, by sweeping the UNDERLYING store directly. This must
    // bypass the normal get()/scan() path so it cannot trigger any adaptive
    // behavior (e.g. AHSE migration). Called after on_load_complete(), before
    // timing, only when the harness --warmup flag is set. Default: no-op.
    virtual void prewarm() {}

    // Anti-optimization sink. Reading it forces the compiler to keep the
    // materialization work performed by get()/scan().
    uint64_t checksum() const { return checksum_; }

protected:
    // Fold a value's bytes into the checksum so materialization is observable.
    inline void materialize(const char* data, size_t len) {
        uint64_t h = checksum_;
        for (size_t i = 0; i < len; ++i) {
            h = h * 1099511628211ull ^ static_cast<unsigned char>(data[i]);
        }
        checksum_ = h;
    }
    inline void materialize(const std::string& v) { materialize(v.data(), v.size()); }

    uint64_t checksum_ = 1469598103934665603ull; // FNV offset basis
};
