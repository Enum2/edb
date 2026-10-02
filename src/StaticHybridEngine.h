#pragma once
//
// StaticHybridEngine.h — the "always maintain both indexes" strawman.
//
// This is Experiment 3: it quantifies what adaptivity saves versus naively
// keeping RocksDB and LMDB in lockstep forever. There is no AdaptiveBrain, no
// Q*-Gate, no drop logic. Dual-write is active from the first put(); the LMDB
// index is built once during on_load_complete() (warmup, NOT timed) so this is
// the most favorable framing for static-hybrid — it isolates the ongoing
// dual-write tax from the one-time build cost.
//
// Reads and scans are always served from LMDB (is_lmdb_ready is effectively
// always true). This is the direct evidentiary answer to Reviewer #1 W4 /
// Reviewer #2's "why not always maintain both".
//
#include <cstdint>
#include "Engine.h"
#include "RocksDBConfig.h"
#include "LmdbUtil.h"
#include <rocksdb/db.h>
#include <lmdb.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

class StaticHybridEngine : public Engine {
public:
    StaticHybridEngine(long long total_keys,
                       const std::string& rocks_path = "data_static_rocks",
                       const std::string& lmdb_path = "data_static_lmdb",
                       size_t map_size = 0)
        : rocks_path_(rocks_path), lmdb_path_(lmdb_path) {
        if (map_size == 0) map_size = lmdbutil::mapsize_for(total_keys, 1024);
        std::filesystem::remove_all(rocks_path_);
        std::filesystem::create_directories(rocks_path_);
        rocksdb::Status s = ahse::open_rocksdb(rocks_path_, rocks_db_);
        if (!s.ok()) throw std::runtime_error("StaticHybrid RocksDB open failed: " + s.ToString());

        std::filesystem::remove_all(lmdb_path_);
        std::filesystem::create_directories(lmdb_path_);
        if (mdb_env_create(&env_) != 0) throw std::runtime_error("mdb_env_create failed");
        mdb_env_set_mapsize(env_, map_size);
        if (mdb_env_open(env_, lmdb_path_.c_str(), MDB_NOSYNC, 0664) != 0)
            throw std::runtime_error("mdb_env_open failed");
        MDB_txn* txn;
        mdb_txn_begin(env_, nullptr, 0, &txn);
        mdb_dbi_open(txn, nullptr, MDB_CREATE, &dbi_);
        mdb_txn_commit(txn);
    }

    ~StaticHybridEngine() override { if (env_) mdb_env_close(env_); }

    std::string name() const override { return "static_hybrid"; }

    // Reads are served from LMDB once ready_, so warm the B+-tree mmap.
    void prewarm() override { lmdbutil::lmdb_prewarm(env_, dbi_); }

    // Build the LMDB mirror once, off the clock (after flushing RocksDB).
    void on_load_complete() override {
        rocksdb::FlushOptions fo; fo.wait = true;
        rocks_db_->Flush(fo);
        std::unique_ptr<rocksdb::Iterator> it(rocks_db_->NewIterator(rocksdb::ReadOptions()));
        MDB_txn* txn;
        mdb_txn_begin(env_, nullptr, 0, &txn);
        long long n = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            MDB_val k{it->key().size(), const_cast<char*>(it->key().data())};
            MDB_val v{it->value().size(), const_cast<char*>(it->value().data())};
            mdb_put(txn, dbi_, &k, &v, 0);
            if (++n % 10000 == 0) { mdb_txn_commit(txn); mdb_txn_begin(env_, nullptr, 0, &txn); }
        }
        mdb_txn_commit(txn);
        ready_ = true;
    }

    void put(const std::string& key, const std::string& value) override {
        rocks_db_->Put(rocksdb::WriteOptions(), key, value); // ground truth first
        MDB_txn* txn;                                        // then always LMDB
        if (mdb_txn_begin(env_, nullptr, 0, &txn) == 0) {
            MDB_val k{key.size(), const_cast<char*>(key.data())};
            MDB_val v{value.size(), const_cast<char*>(value.data())};
            mdb_put(txn, dbi_, &k, &v, 0);
            mdb_txn_commit(txn);
        }
    }

    bool get(const std::string& key) override {
        if (ready_) {
            MDB_txn* txn;
            if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) == 0) {
                MDB_val k{key.size(), const_cast<char*>(key.data())};
                MDB_val v;
                bool found = (mdb_get(txn, dbi_, &k, &v) == 0);
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
        uint64_t count = 0;
        if (ready_) {
            MDB_txn* txn;
            if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) == 0) {
                MDB_cursor* cursor;
                if (mdb_cursor_open(txn, dbi_, &cursor) == 0) {
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
            return count;
        }
        std::unique_ptr<rocksdb::Iterator> it(rocks_db_->NewIterator(rocksdb::ReadOptions()));
        for (it->Seek(start_key);
             it->Valid() && it->key().compare(end_key) <= 0;
             it->Next()) {
            rocksdb::Slice v = it->value();
            materialize(v.data(), v.size());
            ++count;
        }
        return count;
    }

private:
    std::string rocks_path_, lmdb_path_;
    std::unique_ptr<rocksdb::DB> rocks_db_;
    MDB_env* env_ = nullptr;
    MDB_dbi  dbi_{};
    bool ready_ = false;
};
