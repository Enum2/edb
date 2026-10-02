#pragma once
#include <rocksdb/db.h>
#include <lmdb.h>
#include <iostream>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include "WorkloadMonitor.h"
#include "TelemetryLogger.h"

inline TelemetryLogger telemetry_logger;

class StorageManager {
private:
    rocksdb::DB* rocks_db;
    MDB_env* lmdb_env;
    MDB_dbi lmdb_dbi;
    std::atomic<bool> is_lmdb_ready{false};
    std::atomic<bool> is_migrating{false};

public:
    WorkloadMonitor monitor{10000000}; 
    std::atomic<double> cumulative_latency{0.0};
    std::atomic<int> latency_samples{0};

    StorageManager() {
        rocksdb::Options options;
        options.create_if_missing = true;
        rocksdb::DB::Open(options, "data_rocks", &rocks_db);
        std::filesystem::create_directory("data_lmdb");
        mdb_env_create(&lmdb_env);
        mdb_env_set_mapsize(lmdb_env, 1048576000); 
        mdb_env_open(lmdb_env, "data_lmdb", MDB_NOSYNC, 0664);
        MDB_txn* txn;
        mdb_txn_begin(lmdb_env, NULL, 0, &txn);
        mdb_dbi_open(txn, NULL, 0, &lmdb_dbi);
        mdb_txn_commit(txn);
    }
    ~StorageManager() { delete rocks_db; mdb_env_close(lmdb_env); }

    bool is_lmdb_active() const { return is_lmdb_ready.load(); }
    bool is_currently_migrating() const { return is_migrating.load(); }

    void Put(std::string key, std::string value) {
        rocks_db->Put(rocksdb::WriteOptions(), key, value);

        if (monitor.drop_index_fire.exchange(false)) {
            is_lmdb_ready.store(false); 
            std::cout << "[SYSTEM] 🗑️ Dropping LMDB Index to save RAM.\n";
            MDB_txn* drop_txn;
            if (mdb_txn_begin(lmdb_env, NULL, 0, &drop_txn) == 0) {
                mdb_drop(drop_txn, lmdb_dbi, 0); 
                mdb_txn_commit(drop_txn);
            }
        }
        if (monitor.predictive_fire.load() && !is_lmdb_ready.load()) {
            bool expected = false;
            if (is_migrating.compare_exchange_strong(expected, true)) {
                std::thread(&StorageManager::build_lmdb_index, this).detach();
            }
        }
        if (is_lmdb_ready.load()) {
            MDB_txn* txn;
            mdb_txn_begin(lmdb_env, NULL, 0, &txn);
            MDB_val k = {key.size(), (void*)key.data()}; 
            MDB_val v = {value.size(), (void*)value.data()};
            mdb_put(txn, lmdb_dbi, &k, &v, 0); 
            mdb_txn_commit(txn);
        }
        monitor.record_write();
    }

    void Scan(std::string start_key, std::string end_key) {
        monitor.record_read();
        auto t_start = std::chrono::high_resolution_clock::now();
        bool used_lmdb = false;

        if (monitor.reactive_fire.load() && !is_lmdb_ready.load()) {
            bool expected = false;
            if (is_migrating.compare_exchange_strong(expected, true)) {
                std::thread(&StorageManager::build_lmdb_index, this).detach();
            }
        }

        if (is_lmdb_ready.load()) {
            used_lmdb = true;
            MDB_txn* txn;
            mdb_txn_begin(lmdb_env, NULL, MDB_RDONLY, &txn);
            MDB_cursor* cursor;
            mdb_cursor_open(txn, lmdb_dbi, &cursor);
            MDB_val k = {start_key.size(), (void*)start_key.data()};
            MDB_val v;
            if (mdb_cursor_get(cursor, &k, &v, MDB_SET_RANGE) == 0) {
                while (mdb_cursor_get(cursor, &k, &v, MDB_NEXT) == 0) {
                    std::string current_key((char*)k.mv_data, k.mv_size);
                    if (current_key > end_key) break;
                    volatile auto val = v.mv_data; 
                }
            }
            mdb_cursor_close(cursor);
            mdb_txn_abort(txn);
        } else {
            rocksdb::Iterator* it = rocks_db->NewIterator(rocksdb::ReadOptions());
            for (it->Seek(start_key); it->Valid() && it->key().compare(end_key) <= 0; it->Next()) {
                volatile auto val = it->value();
            }
            delete it;
        }

        auto t_end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count() / 1000.0;
        monitor.record_read_latency(used_lmdb, ms);
        
        double current_total = cumulative_latency.load(std::memory_order_relaxed);
        cumulative_latency.store(current_total + ms, std::memory_order_relaxed);
        latency_samples.fetch_add(1, std::memory_order_relaxed);
    }

    void build_lmdb_index() {
        std::cout << "[MIGRATION] ⏸️ Phase 1: Snapping RocksDB state...\n";
        auto mig_start = std::chrono::high_resolution_clock::now();
        const rocksdb::Snapshot* snapshot = rocks_db->GetSnapshot();
        std::cout << "[MIGRATION] 🔀 Phase 2: Dual-write enabled. Building index...\n";

        rocksdb::ReadOptions read_opts;
        read_opts.snapshot = snapshot;
        rocksdb::Iterator* it = rocks_db->NewIterator(read_opts);

        MDB_txn* txn;
        mdb_txn_begin(lmdb_env, NULL, 0, &txn);
        int count = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            MDB_val k, v;
            k.mv_size = it->key().size(); k.mv_data = (void*)it->key().data();
            v.mv_size = it->value().size(); v.mv_data = (void*)it->value().data();
            mdb_put(txn, lmdb_dbi, &k, &v, MDB_NOOVERWRITE);
            count++;
            if (count % 10000 == 0) {
                mdb_txn_commit(txn);
                mdb_txn_begin(lmdb_env, NULL, 0, &txn);
            }
        }
        mdb_txn_commit(txn);
        delete it;
        rocks_db->ReleaseSnapshot(snapshot);

        auto mig_end = std::chrono::high_resolution_clock::now();
        double mig_time = std::chrono::duration_cast<std::chrono::milliseconds>(mig_end - mig_start).count();
        std::cout << "[MIGRATION] ✅ Phase 3: LMDB Built (" << count << " keys) in " << mig_time << " ms.\n";

        monitor.brain.gate().calibrator().record_migration(count, mig_time);
        is_lmdb_ready.store(true);
        is_migrating.store(false); 
    }
};
