#pragma once
//
// AHATreeEngine.h — AHA-inspired baseline (Xing & Aref, arXiv:2406.08746),
// re-implemented behind the Engine interface.
//
// Faithful to the mechanism the paper describes in Eq. 1:
//   * keyspace partitioned into fixed 1000-key segments;
//   * a segment migrates from RocksDB (COLD_LSM) into an in-memory
//     absl::btree_map (HOT_BPLUS) once, for that segment:
//         R/(R+W) > 0.7   AND   R * 0.04 > 1000 * 0.02
//   * once hot, writes dual-write (RocksDB + btree) and reads/scans are served
//     from the btree.
//
// Fidelity fixes vs. the previous header:
//   * absl::btree_map (the structure the paper names), not std::map;
//   * the EXACT Eq. 1 constants (0.04 / 0.02), not invented ones;
//   * canonical key routing via kf::key_to_int, so keys no longer all collapse
//     into segment 0.
//
// This is an approximation of AHA's segment-level adaptation; it does NOT model
// the real AHA-tree's buffer-tree morphing. That limitation is disclosed in the
// paper's baseline description.
//
#include <chrono>
#include <cstdint>
#include <string>
#include <mutex>
#include "Engine.h"
#include "RocksDBConfig.h"
#include "Common.h"
#include <rocksdb/db.h>
#include <absl/container/btree_map.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

class AHATreeEngine : public Engine {
private:
    enum class SegmentState { COLD_LSM, HOT_BPLUS };

    struct Segment {
        int id;
        std::atomic<SegmentState> state{SegmentState::COLD_LSM};
        std::atomic<uint64_t> read_count{0};
        std::atomic<uint64_t> write_count{0};
        std::shared_mutex rw_lock;
        absl::btree_map<std::string, std::string> hot_index;
        explicit Segment(int i) : id(i) {}
    };

    static constexpr uint64_t KEYS_PER_SEGMENT = 1000;

    std::unique_ptr<rocksdb::DB> db_;
    std::string path_;
    std::vector<std::unique_ptr<Segment>> segments_;
    std::atomic<bool> running_{true};
    std::atomic<uint64_t> morph_count_{0};
    std::thread monitor_;

    int segment_of(const std::string& key) const {
        long long id = kf::key_to_int(key);
        if (id < 0) return 0;
        long long seg = id / (long long)KEYS_PER_SEGMENT;
        if (seg < 0) return 0;
        if (seg >= (long long)segments_.size()) return (int)segments_.size() - 1;
        return (int)seg;
    }

    void morph(int seg_id) {
        auto& seg = *segments_[seg_id];
        std::unique_lock<std::shared_mutex> lock(seg.rw_lock);
        if (seg.state.load() == SegmentState::HOT_BPLUS) return;

        std::string start_key = kf::make_key((long long)seg_id * KEYS_PER_SEGMENT);
        std::string end_key   = kf::make_key((long long)(seg_id + 1) * KEYS_PER_SEGMENT - 1);

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
        for (it->Seek(start_key);
             it->Valid() && it->key().compare(end_key) <= 0;
             it->Next()) {
            seg.hot_index[it->key().ToString()] = it->value().ToString();
        }
        seg.state.store(SegmentState::HOT_BPLUS);
        morph_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void monitor_loop() {
        using namespace std::chrono_literals;
        while (running_.load()) {
            std::this_thread::sleep_for(500ms);
            if (!running_.load()) break;
            for (auto& segp : segments_) {
                Segment& seg = *segp;
                if (seg.state.load() != SegmentState::COLD_LSM) continue;
                uint64_t reads  = seg.read_count.exchange(0);
                uint64_t writes = seg.write_count.exchange(0);
                uint64_t total  = reads + writes;
                if (total == 0) continue;
                double read_ratio = (double)reads / (double)total;
                // Paper Eq. 1, verbatim constants.
                bool benefit = (reads * 0.04) > (KEYS_PER_SEGMENT * 0.02);
                if (read_ratio > 0.7 && benefit) {
                    std::thread(&AHATreeEngine::morph, this, seg.id).detach();
                }
            }
        }
    }

public:
    explicit AHATreeEngine(long long total_expected_keys,
                           const std::string& path = "data_aha_rocks")
        : path_(path) {
        int num_segments = (int)(total_expected_keys / (long long)KEYS_PER_SEGMENT) + 1;
        segments_.reserve(num_segments);
        for (int i = 0; i < num_segments; ++i)
            segments_.push_back(std::make_unique<Segment>(i));

        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
        rocksdb::Status s = ahse::open_rocksdb(path_, db_);
        if (!s.ok()) throw std::runtime_error("AHA RocksDB open failed: " + s.ToString());

        monitor_ = std::thread(&AHATreeEngine::monitor_loop, this);
    }

    ~AHATreeEngine() override {
        running_.store(false);
        if (monitor_.joinable()) monitor_.join();
    }

    std::string name() const override { return "aha"; }

    void prewarm() override { ahse::rocks_prewarm(db_.get()); }
    uint64_t morph_count() const { return morph_count_.load(); }

    void on_load_complete() override {
        rocksdb::FlushOptions fo; fo.wait = true;
        db_->Flush(fo);
    }

    void put(const std::string& key, const std::string& value) override {
        Segment& seg = *segments_[segment_of(key)];
        seg.write_count.fetch_add(1, std::memory_order_relaxed);
        db_->Put(rocksdb::WriteOptions(), key, value);        // always persist to LSM
        if (seg.state.load() == SegmentState::HOT_BPLUS) {    // consistency guard
            std::unique_lock<std::shared_mutex> lock(seg.rw_lock);
            if (seg.state.load() == SegmentState::HOT_BPLUS)
                seg.hot_index[key] = value;
        }
    }

    bool get(const std::string& key) override {
        Segment& seg = *segments_[segment_of(key)];
        seg.read_count.fetch_add(1, std::memory_order_relaxed);
        if (seg.state.load() == SegmentState::HOT_BPLUS) {
            std::shared_lock<std::shared_mutex> lock(seg.rw_lock);
            if (seg.state.load() == SegmentState::HOT_BPLUS) {
                auto it = seg.hot_index.find(key);
                if (it != seg.hot_index.end()) { materialize(it->second); return true; }
                return false;
            }
        }
        std::string value;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, &value);
        if (s.ok()) { materialize(value); return true; }
        return false;
    }

    uint64_t scan(const std::string& start_key, const std::string& end_key) override {
        Segment& seg = *segments_[segment_of(start_key)];
        seg.read_count.fetch_add(1, std::memory_order_relaxed);
        uint64_t count = 0;

        if (seg.state.load() == SegmentState::HOT_BPLUS) {
            std::shared_lock<std::shared_mutex> lock(seg.rw_lock);
            if (seg.state.load() == SegmentState::HOT_BPLUS) {
                auto it_start = seg.hot_index.lower_bound(start_key);
                for (auto it = it_start; it != seg.hot_index.end() && it->first <= end_key; ++it) {
                    materialize(it->second);
                    ++count;
                }
                return count;
            }
        }
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
};
