#pragma once
//
// RocksDBEngine.h — the pure RocksDB (LSM-tree) baseline.
//
// RocksDB 11.8 API: DB::Open takes std::unique_ptr<DB>*, so ownership is held
// in a unique_ptr (no manual delete). Reads use Get(); scans use an iterator.
// Both materialize the bytes they touch.
//
#include <cstdint>
#include "Engine.h"
#include "RocksDBConfig.h"
#include <rocksdb/db.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

class RocksDBEngine : public Engine {
public:
    explicit RocksDBEngine(const std::string& path = "data_rocks_baseline")
        : path_(path) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
        rocksdb::Status s = ahse::open_rocksdb(path_, db_);
        if (!s.ok()) throw std::runtime_error("RocksDB open failed: " + s.ToString());
    }

    std::string name() const override { return "rocksdb"; }

    std::string debug_state() override { return ahse::rocks_compaction_state(db_.get()); }
    void prewarm() override { ahse::rocks_prewarm(db_.get()); }

    // Flush the memtable to SSTables after load so range scans pay the real
    // LSM multi-level cost (otherwise small datasets stay in the memtable and
    // scans are unrealistically cheap).
    void on_load_complete() override {
        rocksdb::FlushOptions fo; fo.wait = true;
        db_->Flush(fo);
    }

    void put(const std::string& key, const std::string& value) override {
        db_->Put(rocksdb::WriteOptions(), key, value);
    }

    bool get(const std::string& key) override {
        std::string value;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, &value);
        if (s.ok()) { materialize(value); return true; }
        return false;
    }

    uint64_t scan(const std::string& start_key, const std::string& end_key) override {
        uint64_t count = 0;
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
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
    std::string path_;
    std::unique_ptr<rocksdb::DB> db_;
};
