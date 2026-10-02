#pragma once
//
// LMDBEngine.h — standalone LMDB (memory-mapped B+-tree) baseline.
//
// This is Experiment 2b: an independent read-optimized baseline so the paper's
// comparison is not solely against the author-built AHA reimplementation
// (Reviewer #2 D6 / O6). All reads and writes go directly to LMDB; there is no
// RocksDB, no migration, no dual-write.
//
// Scan fix: the old code did MDB_SET_RANGE then immediately MDB_NEXT, which
// SKIPPED the first in-range key. Here we process the SET_RANGE hit first, then
// advance — so LMDB visits exactly the same key set as the RocksDB iterator.
//
#include <cstdint>
#include "Engine.h"
#include "LmdbUtil.h"
#include <lmdb.h>
#include <filesystem>
#include <stdexcept>
#include <string>

class LMDBEngine : public Engine {
public:
    explicit LMDBEngine(const std::string& path = "data_lmdb_only",
                        size_t map_size = (size_t)16 * 1024 * 1024 * 1024)
        : path_(path) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
        check(mdb_env_create(&env_), "env_create");
        check(mdb_env_set_mapsize(env_, map_size), "set_mapsize");
        // MDB_NOSYNC: safe here because this is a benchmark; matches the paper's
        // stated LMDB open flags.
        check(mdb_env_open(env_, path_.c_str(), MDB_NOSYNC, 0664), "env_open");
        MDB_txn* txn;
        check(mdb_txn_begin(env_, nullptr, 0, &txn), "txn_begin(open)");
        check(mdb_dbi_open(txn, nullptr, MDB_CREATE, &dbi_), "dbi_open");
        check(mdb_txn_commit(txn), "txn_commit(open)");
    }

    ~LMDBEngine() override {
        if (env_) mdb_env_close(env_);
    }

    std::string name() const override { return "lmdb"; }

    void prewarm() override { lmdbutil::lmdb_prewarm(env_, dbi_); }

    void put(const std::string& key, const std::string& value) override {
        MDB_txn* txn;
        if (mdb_txn_begin(env_, nullptr, 0, &txn) != 0) return;
        MDB_val k{key.size(), const_cast<char*>(key.data())};
        MDB_val v{value.size(), const_cast<char*>(value.data())};
        mdb_put(txn, dbi_, &k, &v, 0);
        mdb_txn_commit(txn);
    }

    bool get(const std::string& key) override {
        MDB_txn* txn;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != 0) return false;
        MDB_val k{key.size(), const_cast<char*>(key.data())};
        MDB_val v;
        bool found = (mdb_get(txn, dbi_, &k, &v) == 0);
        if (found) materialize(static_cast<const char*>(v.mv_data), v.mv_size);
        mdb_txn_abort(txn);
        return found;
    }

    uint64_t scan(const std::string& start_key, const std::string& end_key) override {
        MDB_txn* txn;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != 0) return 0;
        MDB_cursor* cursor;
        if (mdb_cursor_open(txn, dbi_, &cursor) != 0) { mdb_txn_abort(txn); return 0; }

        uint64_t count = 0;
        MDB_val k{start_key.size(), const_cast<char*>(start_key.data())};
        MDB_val v;
        int rc = mdb_cursor_get(cursor, &k, &v, MDB_SET_RANGE);
        while (rc == 0) {
            if (lmdbutil::key_gt(k, end_key)) break;  // inclusive upper bound
            materialize(static_cast<const char*>(v.mv_data), v.mv_size);
            ++count;
            rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        mdb_txn_abort(txn);
        return count;
    }

private:
    static void check(int rc, const char* what) {
        if (rc != 0) throw std::runtime_error(std::string("LMDB ") + what + ": " + mdb_strerror(rc));
    }

    std::string path_;
    MDB_env* env_ = nullptr;
    MDB_dbi  dbi_{};
};
