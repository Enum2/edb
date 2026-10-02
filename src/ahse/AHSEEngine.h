#pragma once
//
// AHSEEngine.h — the Adaptive Hybrid Storage Engine (rewrite of StorageManager).
//
// RocksDB (LSM) is the ground-truth store; LMDB (mmap B+-tree) is a derived,
// reconstructible secondary index provisioned on demand by the AdaptiveBrain.
//
// Fixes vs. the submitted StorageManager.h:
//   * dataset size is a constructor argument threaded to the monitor/brain
//     (was hard-coded 10,000,000);
//   * scans materialize values (checksum) — no more optimized-away reads;
//   * point reads (get) can also trigger a reactive migration and are served
//     from LMDB when ready, so read-heavy (non-scan) phases benefit too;
//   * only range-scan latencies feed the Q* EMAs (paper's "scan latency");
//   * build_lmdb_index() performs the idempotent mdb_drop clear (§3.5 stage 1)
//     so rebuilds are always safe;
//   * RocksDB 11.8 unique_ptr ownership; shared options via RocksDBConfig.
//
#include <cstdlib>
#include <cstdint>
#include "../Engine.h"
#include "../RocksDBConfig.h"
#include "../Common.h"
#include "../LmdbUtil.h"
#include "WorkloadMonitor.h"
#include "TelemetryLogger.h"
#include <rocksdb/db.h>
#include <lmdb.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <cstring>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <algorithm>
#include <random>

class AHSEEngine : public Engine {
public:
    AHSEEngine(long long total_keys,
               const std::string& rocks_path = "data_rocks",
               const std::string& lmdb_path = "data_lmdb",
               const std::string& telemetry_path = "telemetry.csv",
               bool verbose = false,
               size_t map_size = 0)
        : rocks_path_(rocks_path),
          lmdb_path_(lmdb_path),
          logger_(telemetry_path) {
        if (map_size == 0) map_size = lmdbutil::mapsize_for(total_keys, 1024);
        // RocksDB
        std::filesystem::remove_all(rocks_path_);
        std::filesystem::create_directories(rocks_path_);
        rocksdb::Status s = ahse::open_rocksdb(rocks_path_, rocks_db_);
        if (!s.ok()) throw std::runtime_error("AHSE RocksDB open failed: " + s.ToString());

        // LMDB: map size scales with dataset + record size (virtual reservation).
        std::filesystem::remove_all(lmdb_path_);
        std::filesystem::create_directories(lmdb_path_);
        if (mdb_env_create(&lmdb_env_) != 0) throw std::runtime_error("mdb_env_create failed");
        mdb_env_set_mapsize(lmdb_env_, map_size);
        if (mdb_env_open(lmdb_env_, lmdb_path_.c_str(), MDB_NOSYNC, 0664) != 0)
            throw std::runtime_error("mdb_env_open failed");
        MDB_txn* txn;
        mdb_txn_begin(lmdb_env_, nullptr, 0, &txn);
        mdb_dbi_open(txn, nullptr, MDB_CREATE, &lmdb_dbi_);
        mdb_txn_commit(txn);

        // Monitor is constructed last: it starts a background thread that reads
        // is_lmdb_ready_ / is_migrating_, which are already-constructed members.
        monitor_ = std::make_unique<WorkloadMonitor>(
            total_keys, &logger_, &is_lmdb_ready_, &is_migrating_, 500, verbose);
        // Diagnostic: AHSE_NO_MONITOR bypasses per-op monitor bookkeeping so we
        // can measure the monitoring tax in isolation. Default path unchanged.
        monitoring_ = (std::getenv("AHSE_NO_MONITOR") == nullptr);
    }

    ~AHSEEngine() override {
        // Order matters: a detached build thread calls monitor_->brain
        // (record_migration / on_migration_complete) right before it clears
        // is_migrating_. Wait for any in-flight build to finish FIRST, then tear
        // down the monitor — otherwise resetting monitor_ while a build is still
        // running is a use-after-free (transient segfault under teardown races).
        // No new build can start here: maybe_start_migration() is only called
        // from foreground get/put/scan, which have already stopped.
        while (is_migrating_.load()) std::this_thread::yield(); // let migration finish first
        monitor_.reset();                 // then join the monitor thread
        if (lmdb_env_) mdb_env_close(lmdb_env_);
    }

    std::string name() const override { return "ahse"; }

    std::string debug_state() override { return ahse::rocks_compaction_state(rocks_db_.get()); }
    // Warm the RocksDB store directly (raw iterator) — never touches the monitor
    // or migration path, so warm-up cannot pre-build the LMDB index.
    void prewarm() override { ahse::rocks_prewarm(rocks_db_.get()); }

    void on_load_complete() override {
        rocksdb::FlushOptions fo; fo.wait = true;
        rocks_db_->Flush(fo);
    }

    bool is_lmdb_active() const { return is_lmdb_ready_.load(); }
    bool is_currently_migrating() const { return is_migrating_.load(); }
    WorkloadMonitor& monitor() { return *monitor_; }

    void put(const std::string& key, const std::string& value) override {
        rocks_db_->Put(rocksdb::WriteOptions(), key, value);

        if (monitoring_ && monitor_->drop_index_fire.exchange(false)) {
            is_lmdb_ready_.store(false);
            MDB_txn* dtxn;
            if (mdb_txn_begin(lmdb_env_, nullptr, 0, &dtxn) == 0) {
                mdb_drop(dtxn, lmdb_dbi_, 0);
                mdb_txn_commit(dtxn);
            }
        }
        if (monitoring_ && monitor_->predictive_fire.load() && !is_lmdb_ready_.load())
            maybe_start_migration();

        // Consistency-preserving dual-write:
        //  * index live  -> write LMDB directly (freshest wins);
        //  * mid-build    -> record the key so the post-build catch-up applies
        //                    its current RocksDB value (no LMDB contention with
        //                    the fast MDB_APPEND bulk load).
        if (is_lmdb_ready_.load()) {
            lmdb_put_direct(key, value);
        } else if (is_migrating_.load()) {
            std::lock_guard<std::mutex> lk(pending_mu_);
            if (is_lmdb_ready_.load()) lmdb_put_direct(key, value); // published while waiting
            else pending_.insert(key);
        }
        if (monitoring_) monitor_->record_write();
    }

    bool get(const std::string& key) override {
        if (monitoring_) {
            monitor_->record_read();
            if (monitor_->reactive_fire.load() && !is_lmdb_ready_.load())
                maybe_start_migration();
        }

        if (is_lmdb_ready_.load()) {
            MDB_txn* txn;
            if (mdb_txn_begin(lmdb_env_, nullptr, MDB_RDONLY, &txn) == 0) {
                MDB_val k{key.size(), const_cast<char*>(key.data())};
                MDB_val v;
                bool found = (mdb_get(txn, lmdb_dbi_, &k, &v) == 0);
                if (found) materialize(static_cast<const char*>(v.mv_data), v.mv_size);
                mdb_txn_abort(txn);
                return found;
            }
        }
        std::string value;
        rocksdb::Status s = rocks_db_->Get(rocksdb::ReadOptions(), key, &value);
        if (s.ok()) { materialize(value); return true; }
        return false;
    }

    uint64_t scan(const std::string& start_key, const std::string& end_key) override {
        if (monitoring_) monitor_->record_read();
        auto t0 = timing::clock::now();
        bool used_lmdb = false;
        uint64_t count = 0;

        if (monitoring_ && monitor_->reactive_fire.load() && !is_lmdb_ready_.load())
            maybe_start_migration();

        if (is_lmdb_ready_.load()) {
            used_lmdb = true;
            MDB_txn* txn;
            if (mdb_txn_begin(lmdb_env_, nullptr, MDB_RDONLY, &txn) == 0) {
                MDB_cursor* cursor;
                if (mdb_cursor_open(txn, lmdb_dbi_, &cursor) == 0) {
                    MDB_val k{start_key.size(), const_cast<char*>(start_key.data())};
                    MDB_val v;
                    int rc = mdb_cursor_get(cursor, &k, &v, MDB_SET_RANGE);
                    while (rc == 0) {
                        if (lmdbutil::key_gt(k, end_key)) break;
                        materialize(static_cast<const char*>(v.mv_data), v.mv_size);
                        ++count;
                        rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
                    }
                    mdb_cursor_close(cursor);
                }
                mdb_txn_abort(txn);
            }
        } else {
            std::unique_ptr<rocksdb::Iterator> it(rocks_db_->NewIterator(rocksdb::ReadOptions()));
            for (it->Seek(start_key);
                 it->Valid() && it->key().compare(end_key) <= 0;
                 it->Next()) {
                rocksdb::Slice sv = it->value();
                materialize(sv.data(), sv.size());
                ++count;
            }
        }

        double ms = timing::ms_since(t0);
        if (monitoring_) monitor_->record_scan_latency(used_lmdb, ms);
        return count;
    }

private:
    void lmdb_put_direct(const std::string& key, const std::string& value) {
        MDB_txn* txn;
        if (mdb_txn_begin(lmdb_env_, nullptr, 0, &txn) == 0) {
            MDB_val k{key.size(), const_cast<char*>(key.data())};
            MDB_val v{value.size(), const_cast<char*>(value.data())};
            mdb_put(txn, lmdb_dbi_, &k, &v, 0); // overwrite: freshest wins
            mdb_txn_commit(txn);
        }
    }

    void maybe_start_migration() {
        bool expected = false;
        if (is_migrating_.compare_exchange_strong(expected, true))
            std::thread(&AHSEEngine::build_lmdb_index, this).detach();
    }

    // §3.5 migration: idempotent clear -> consistent snapshot -> batched bulk
    // copy -> calibrate alpha -> atomic flag flip.
    void build_lmdb_index() {
        auto mig_start = timing::clock::now();

        // Stage 1: idempotent clear.
        {
            MDB_txn* dtxn;
            if (mdb_txn_begin(lmdb_env_, nullptr, 0, &dtxn) == 0) {
                mdb_drop(dtxn, lmdb_dbi_, 0);
                mdb_txn_commit(dtxn);
            }
        }

        // Stage 2: consistent snapshot of RocksDB. Mark the start of the
        // migration transition window (snapshot -> flag flip). Writes that
        // arrive during the window are buffered in pending_ (see put()) rather
        // than written to LMDB, so the bulk load below is the sole LMDB writer
        // and can use the fast sequential MDB_APPEND path.
        build_start_ns_.store(now_ns());
        const rocksdb::Snapshot* snapshot = rocks_db_->GetSnapshot();
        rocksdb::ReadOptions ro;
        ro.snapshot = snapshot;
        std::unique_ptr<rocksdb::Iterator> it(rocks_db_->NewIterator(ro));

        // Stage 3: batched bulk copy.
        MDB_txn* txn;
        mdb_txn_begin(lmdb_env_, nullptr, 0, &txn);
        long long count = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            MDB_val k{it->key().size(), const_cast<char*>(it->key().data())};
            MDB_val v{it->value().size(), const_cast<char*>(it->value().data())};
            // MDB_APPEND: keys arrive in sorted order from the RocksDB iterator,
            // so this is a sequential B+-tree fill (no search/rebalance) — far
            // faster than random mdb_put. Safe because the build thread is the
            // only LMDB writer during the window (concurrent writes are buffered).
            mdb_put(txn, lmdb_dbi_, &k, &v, MDB_APPEND);
            if (++count % 50000 == 0) {
                mdb_txn_commit(txn);
                mdb_txn_begin(lmdb_env_, nullptr, 0, &txn);
            }
        }
        mdb_txn_commit(txn);
        it.reset();
        rocks_db_->ReleaseSnapshot(snapshot);

        double mig_ms = timing::ms_since(mig_start);
        last_build_keys_.store(count);
        last_build_ms_.store(mig_ms);

        // Stage 4: calibrate alpha from the bulk-build cost.
        monitor_->brain.gate().calibrator().record_migration(count, mig_ms);

        // Stage 5: catch-up + atomic publish. Under pending_mu_, apply every
        // key written during the window (current RocksDB value), then flip
        // is_lmdb_ready. Writers that raced the flip re-check the flag under the
        // same lock and dual-write directly, so no write is lost.
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            if (!pending_.empty()) {
                MDB_txn* ct;
                if (mdb_txn_begin(lmdb_env_, nullptr, 0, &ct) == 0) {
                    for (const auto& key : pending_) {
                        std::string val;
                        if (rocks_db_->Get(rocksdb::ReadOptions(), key, &val).ok()) {
                            MDB_val k{key.size(), const_cast<char*>(key.data())};
                            MDB_val v{val.size(), const_cast<char*>(val.data())};
                            mdb_put(ct, lmdb_dbi_, &k, &v, 0);
                        }
                    }
                    mdb_txn_commit(ct);
                }
                pending_.clear();
            }
            is_lmdb_ready_.store(true);
        }
        build_end_ns_.store(now_ns());   // end of transition window
        monitor_->brain.on_migration_complete(); // one completed build window (this thread)
        is_migrating_.store(false);
    }

public:
    static long long now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    long long last_build_keys() const { return last_build_keys_.load(); }
    double    last_build_ms()   const { return last_build_ms_.load(); }
    // Counter split: gate votes (per-tick) vs completed physical build windows.
    int gate_approvals()      const { return monitor_->brain.gate_approvals(); }
    int physical_migrations() const { return monitor_->brain.physical_migrations(); }
    long long build_start_ns()  const { return build_start_ns_.load(); }
    long long build_end_ns()    const { return build_end_ns_.load(); }

    // Test hook: deterministically start a FULL migration, bypassing the
    // Q*-gate (used by the concurrency experiment to create a controlled
    // migration window under write load).
    void force_migration() { maybe_start_migration(); }

    // Consistency audit: RocksDB is ground truth; every RocksDB key must be
    // present in LMDB with an identical value. Reports missing / mismatched
    // entries. Meaningful only once is_lmdb_ready.
    struct ConsistencyReport { long long total = 0, missing = 0, mismatch = 0; };
    ConsistencyReport verify_consistency() {
        ConsistencyReport r;
        MDB_txn* txn;
        if (mdb_txn_begin(lmdb_env_, nullptr, MDB_RDONLY, &txn) != 0) return r;
        std::unique_ptr<rocksdb::Iterator> it(rocks_db_->NewIterator(rocksdb::ReadOptions()));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            r.total++;
            MDB_val k{it->key().size(), const_cast<char*>(const_cast<char*>(it->key().data()))};
            MDB_val v;
            int rc = mdb_get(txn, lmdb_dbi_, &k, &v);
            if (rc != 0) { r.missing++; continue; }
            rocksdb::Slice rv = it->value();
            if (rv.size() != v.mv_size ||
                std::memcmp(rv.data(), v.mv_data, rv.size()) != 0) r.mismatch++;
        }
        mdb_txn_abort(txn);
        return r;
    }

private:
    std::string rocks_path_;
    std::string lmdb_path_;
    std::unique_ptr<rocksdb::DB> rocks_db_;
    MDB_env* lmdb_env_ = nullptr;
    MDB_dbi  lmdb_dbi_{};

    std::atomic<bool> is_lmdb_ready_{false};
    std::atomic<bool> is_migrating_{false};
    bool monitoring_{true};   // AHSE_NO_MONITOR diagnostic bypass (default: on)
    std::mutex pending_mu_;                    // guards pending_ and the publish flip
    std::unordered_set<std::string> pending_;  // keys written during the build window
    std::atomic<long long> last_build_keys_{0};
    std::atomic<double> last_build_ms_{0.0};
    std::atomic<long long> build_start_ns_{0};
    std::atomic<long long> build_end_ns_{0};

    TelemetryLogger logger_;
    std::unique_ptr<WorkloadMonitor> monitor_;
};
