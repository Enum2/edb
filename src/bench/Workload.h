#pragma once
//
// Workload.h — YCSB and custom workload generation.
//
// YCSB A/B/E with an 80/20 hot-set access pattern (approximating Zipf). Keys are
// canonical (kf::make_key), shared by every engine. Scan length is configurable:
// standard YCSB-E draws 1..scan_len; the paper used a fixed 1000-key range, so
// the driver can set fixed_scan_len to reproduce that exactly.
//
#include <numeric>
#include <algorithm>
#include "../Common.h"
#include <string>
#include <vector>
#include <random>
#include <cstdint>

namespace wl {

enum class OpType { READ, UPDATE, SCAN, INSERT, RMW };

struct Op {
    OpType type;
    std::string key;      // start key
    std::string key_end;  // for SCAN (inclusive)
    std::string value;    // for UPDATE / INSERT / RMW
};

struct YcsbConfig {
    int    num_ops    = 0;
    long long num_keys = 0;
    int    read_pct   = 0;
    int    update_pct = 0;
    int    scan_pct   = 0;
    int    insert_pct = 0;
    int    rmw_pct    = 0;     // read-modify-write (Workload F)
    int    scan_len   = 100;   // standard YCSB-E max scan length
    bool   fixed_scan = false; // if true, every scan spans exactly scan_len keys
    unsigned seed     = 42;
    std::string value = "ycsb_payload________________"; // ~28 bytes
};

inline std::vector<Op> generate_ycsb(const YcsbConfig& c) {
    std::vector<Op> out;
    out.reserve(c.num_ops);
    std::mt19937 rng(c.seed);
    std::uniform_int_distribution<int> pct(1, 100);
    long long limit = c.num_keys - c.scan_len - 1;
    if (limit < 1) limit = 1;
    long long hot_hi = std::max<long long>(1, (long long)(limit * 0.20));
    std::uniform_int_distribution<long long> hot(0, hot_hi);
    std::uniform_int_distribution<long long> cold(hot_hi, limit);
    std::uniform_int_distribution<int> hotcold(1, 100);
    std::uniform_int_distribution<int> scanlen(1, std::max(1, c.scan_len));
    long long insert_seq = c.num_keys;

    for (int i = 0; i < c.num_ops; ++i) {
        Op op;
        long long k = (hotcold(rng) <= 80) ? hot(rng) : cold(rng); // 80% -> hot 20%
        int a = pct(rng);
        int t1 = c.read_pct;
        int t2 = t1 + c.update_pct;
        int t3 = t2 + c.scan_pct;
        int t4 = t3 + c.insert_pct;
        if (a <= t1) {
            op.type = OpType::READ;
            op.key = kf::make_key(k);
        } else if (a <= t2) {
            op.type = OpType::UPDATE;
            op.key = kf::make_key(k);
            op.value = c.value;
        } else if (a <= t3) {
            op.type = OpType::SCAN;
            int len = c.fixed_scan ? c.scan_len : scanlen(rng);
            op.key = kf::make_key(k);
            op.key_end = kf::make_key(k + len);
        } else if (a <= t4) {
            op.type = OpType::INSERT;
            op.key = kf::make_key(insert_seq++);
            op.value = c.value;
        } else {
            op.type = OpType::RMW;              // Workload F: read-modify-write
            op.key = kf::make_key(k);
            op.value = c.value;
        }
        out.push_back(std::move(op));
    }
    return out;
}

// Custom benchmark scan spec (integer form; keys built at execution time).
struct ScanSpec { long long start; int len; };

inline std::vector<ScanSpec> generate_scans(int num_scans, long long num_keys,
                                            int scan_len, unsigned seed = 42) {
    std::vector<ScanSpec> out;
    out.reserve(num_scans);
    std::mt19937 rng(seed);
    long long limit = num_keys - scan_len - 1;
    if (limit < 1) limit = 1;
    std::uniform_int_distribution<long long> kd(0, limit);
    for (int i = 0; i < num_scans; ++i) out.push_back({kd(rng), scan_len});
    return out;
}

} // namespace wl
