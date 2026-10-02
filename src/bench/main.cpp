//
// main.cpp — unified AHSE benchmark driver.
//
// One binary drives every engine (rocksdb, lmdb, aha, static_hybrid, ahse)
// through identical call paths, for either YCSB (A/B/E) or the custom
// write+scan benchmark. It captures honest measured latencies (no fabricated
// Q* accounting), writes one raw CSV row per repetition, and prints
// mean +/- 95% CI across repetitions.
//
// Usage:
//   ahse_bench --engine <name> --mode <ycsb-a|ycsb-b|ycsb-e|custom>
//              --keys N --ops M --runs R [--scan-len L] [--fixed-scan]
//              [--out FILE] [--tag TAG] [--verbose]
//
// For true per-run process isolation (peak-RAM measurement), invoke with
// --runs 1 from run_all.sh, which loops one process per repetition.
//
#include <cstdint>
#include "Workload.h"
#include "Metrics.h"
#include "EngineFactory.h"
#include "PeakSampler.h"
#include "../Common.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <numeric>
#include <random>
#include <algorithm>

using namespace std;

// Load-key ordering. Sequential gives RocksDB a perfectly-sorted, well-compacted
// LSM (best case, cheap scans); shuffled reproduces realistic random ingestion,
// which leaves many overlapping SSTable runs and expensive range scans.
static vector<long long> load_order(long long keys, bool shuffle) {
    vector<long long> order(keys);
    std::iota(order.begin(), order.end(), 0LL);
    if (shuffle) { std::mt19937_64 rng(12345); std::shuffle(order.begin(), order.end(), rng); }
    return order;
}

struct Args {
    string engine = "ahse";
    string mode = "ycsb-e";
    long long keys = 500000;
    int ops = 200000;
    int runs = 5;
    int scan_len = 100;
    bool fixed_scan = false;
    int value_size = 1000;   // YCSB record size (~1 KB)
    bool shuffle_load = false; // load in random key order (realistic ingestion)
    bool warmup = false;       // prime cache (prewarm) after load, before timing
    string out = "results.csv";
    string tag = "run";
    bool verbose = false;
};

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        string s = argv[i];
        auto next = [&](const char* name) -> string {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", name); exit(2); }
            return argv[++i];
        };
        if (s == "--engine") a.engine = next("--engine");
        else if (s == "--mode") a.mode = next("--mode");
        else if (s == "--keys") a.keys = stoll(next("--keys"));
        else if (s == "--ops") a.ops = stoi(next("--ops"));
        else if (s == "--runs") a.runs = stoi(next("--runs"));
        else if (s == "--scan-len") a.scan_len = stoi(next("--scan-len"));
        else if (s == "--fixed-scan") a.fixed_scan = true;
        else if (s == "--value-size") a.value_size = stoi(next("--value-size"));
        else if (s == "--shuffle-load") a.shuffle_load = true;
        else if (s == "--warmup") a.warmup = true;
        else if (s == "--out") a.out = next("--out");
        else if (s == "--tag") a.tag = next("--tag");
        else if (s == "--verbose") a.verbose = true;
        else { fprintf(stderr, "unknown arg: %s\n", s.c_str()); exit(2); }
    }
    return a;
}

struct RunResult {
    double exec_ms = 0;
    uint64_t writes = 0, reads = 0, scans = 0;
    metrics::LatencyStats wlat, rlat, slat;
    double ram_mb = 0;
    long long ahse_build_keys = 0;
    double ahse_build_ms = 0;
    int ahse_migrations = 0;
    uint64_t checksum = 0;
};

// YCSB percentages by workload. scan_pct workloads use fixed 1000-key ranges to
// mirror the paper's Workload E definition; the driver flag can override.
static wl::YcsbConfig ycsb_config(const string& mode, long long keys, int ops,
                                  int scan_len, bool fixed_scan) {
    wl::YcsbConfig c;
    c.num_ops = ops; c.num_keys = keys; c.scan_len = scan_len; c.fixed_scan = fixed_scan;
    if (mode == "ycsb-a") { c.read_pct = 50; c.update_pct = 50; }
    else if (mode == "ycsb-b") { c.read_pct = 95; c.update_pct = 5; }
    else if (mode == "ycsb-c") { c.read_pct = 100; }
    else if (mode == "ycsb-d") { c.read_pct = 95; c.insert_pct = 5; }   // read-latest approximated
    else if (mode == "ycsb-e") { c.scan_pct = 95; c.insert_pct = 5; }
    else if (mode == "ycsb-f") { c.read_pct = 50; c.rmw_pct = 50; }     // read-modify-write
    else { fprintf(stderr, "bad ycsb mode %s\n", mode.c_str()); exit(2); }
    return c;
}

static RunResult run_ycsb(const Args& a, int run_idx) {
    string tag = a.tag + "_" + a.engine + "_r" + to_string(run_idx);
    string tel = "runs/" + tag + "_telemetry.csv";
    auto engine = bench::make_engine(a.engine, a.keys, tag, tel, a.verbose, a.value_size);
    PeakSampler peak;

    // Load (untimed).
    std::string load_val = kf::make_value(a.value_size);
    for (long long i : load_order(a.keys, a.shuffle_load))
        engine->put(kf::make_key(i), load_val);
    engine->on_load_complete();
    if (a.warmup) engine->prewarm();   // prime cache before timed window

    bool log_comp = (std::getenv("AHSE_LOG_COMPACTION") != nullptr);
    if (log_comp)
        fprintf(stderr, "[COMPACTION pre-workload] %s | %s keys=%lld run=%d :: %s\n",
                a.engine.c_str(), a.mode.c_str(), a.keys, run_idx, engine->debug_state().c_str());

    wl::YcsbConfig cfg = ycsb_config(a.mode, a.keys, a.ops, a.scan_len, a.fixed_scan);
    cfg.value = kf::make_value(a.value_size);
    auto work = wl::generate_ycsb(cfg);

    vector<double> wlat, rlat, slat;
    wlat.reserve(a.ops); rlat.reserve(a.ops); slat.reserve(a.ops);
    RunResult r;

    auto t0 = timing::clock::now();
    for (const auto& op : work) {
        auto s = timing::clock::now();
        switch (op.type) {
            case wl::OpType::READ:   engine->get(op.key); rlat.push_back(timing::ms_since(s)); r.reads++; break;
            case wl::OpType::SCAN:   engine->scan(op.key, op.key_end); slat.push_back(timing::ms_since(s)); r.scans++; break;
            case wl::OpType::UPDATE:
            case wl::OpType::INSERT: engine->put(op.key, op.value); wlat.push_back(timing::ms_since(s)); r.writes++; break;
            case wl::OpType::RMW:    engine->get(op.key); engine->put(op.key, op.value);
                                     wlat.push_back(timing::ms_since(s)); r.reads++; r.writes++; break;
        }
    }
    r.exec_ms = timing::ms_since(t0);
    peak.stop();
    if (log_comp)
        fprintf(stderr, "[COMPACTION post-workload] %s | %s keys=%lld run=%d exec_ms=%.0f :: %s\n",
                a.engine.c_str(), a.mode.c_str(), a.keys, run_idx, r.exec_ms, engine->debug_state().c_str());
    r.ram_mb = peak.peak_mb();
    r.wlat = metrics::compute(wlat);
    r.rlat = metrics::compute(rlat);
    r.slat = metrics::compute(slat);
    r.checksum = engine->checksum();

    if (auto* ah = dynamic_cast<AHSEEngine*>(engine.get())) {
        r.ahse_build_keys = ah->last_build_keys();
        r.ahse_build_ms = ah->last_build_ms();
        r.ahse_migrations = ah->monitor().brain.successful_migrations();
    }
    return r;
}

static RunResult run_custom(const Args& a, int run_idx) {
    string tag = a.tag + "_" + a.engine + "_r" + to_string(run_idx);
    string tel = "runs/" + tag + "_telemetry.csv";
    auto engine = bench::make_engine(a.engine, a.keys, tag, tel, a.verbose, a.value_size);
    PeakSampler peak;

    RunResult r;
    // Timed write phase (write throughput / latency).
    std::string wval = kf::make_value(a.value_size);
    vector<double> wlat; wlat.reserve(a.keys);
    auto order = load_order(a.keys, a.shuffle_load);
    auto tw = timing::clock::now();
    for (long long i : order) {
        auto s = timing::clock::now();
        engine->put(kf::make_key(i), wval);
        wlat.push_back(timing::ms_since(s));
    }
    double write_ms = timing::ms_since(tw);
    r.writes = a.keys;
    engine->on_load_complete();

    // Timed scan phase (scan throughput / latency).
    int scan_len = a.fixed_scan ? a.scan_len : a.scan_len;
    auto scans = wl::generate_scans(a.ops, a.keys, scan_len);
    vector<double> slat; slat.reserve(a.ops);
    auto ts = timing::clock::now();
    for (const auto& sc : scans) {
        auto s = timing::clock::now();
        engine->scan(kf::make_key(sc.start), kf::make_key(sc.start + sc.len));
        slat.push_back(timing::ms_since(s));
    }
    double scan_ms = timing::ms_since(ts);
    r.scans = a.ops;

    r.exec_ms = write_ms + scan_ms;
    peak.stop();
    r.ram_mb = peak.peak_mb();
    r.wlat = metrics::compute(wlat);
    r.slat = metrics::compute(slat);
    r.checksum = engine->checksum();
    // Store phase throughputs in the exec_ms-adjacent fields via stats: we print
    // throughput below using writes/scan counts and phase times.
    r.rlat.avg = write_ms; // reuse: write phase ms
    r.rlat.p50 = scan_ms;  // reuse: scan phase ms

    if (auto* ah = dynamic_cast<AHSEEngine*>(engine.get())) {
        r.ahse_build_keys = ah->last_build_keys();
        r.ahse_build_ms = ah->last_build_ms();
        r.ahse_migrations = ah->monitor().brain.successful_migrations();
    }
    return r;
}

static void write_header_if_new(const string& path) {
    if (std::filesystem::exists(path)) return;
    std::ofstream f(path, std::ios::app);
    f << "engine,mode,keys,ops,value_size,scan_len,run,exec_ms,writes,reads,scans,"
         "wlat_avg,wlat_p99,rlat_avg,rlat_p99,slat_avg,slat_p50,slat_p95,slat_p99,slat_p999,"
         "ram_mb,ahse_build_keys,ahse_build_ms,ahse_migrations,checksum\n";
}

static void append_row(const string& path, const Args& a, int run, const RunResult& r) {
    std::ofstream f(path, std::ios::app);
    f << a.engine << ',' << a.mode << ',' << a.keys << ',' << a.ops << ','
      << a.value_size << ',' << a.scan_len << ',' << run << ','
      << r.exec_ms << ',' << r.writes << ',' << r.reads << ',' << r.scans << ','
      << r.wlat.avg << ',' << r.wlat.p99 << ',' << r.rlat.avg << ',' << r.rlat.p99 << ','
      << r.slat.avg << ',' << r.slat.p50 << ',' << r.slat.p95 << ',' << r.slat.p99 << ',' << r.slat.p999 << ','
      << r.ram_mb << ',' << r.ahse_build_keys << ',' << r.ahse_build_ms << ',' << r.ahse_migrations << ','
      << r.checksum << '\n';
}

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    if (!bench::engine_exists(a.engine)) { fprintf(stderr, "unknown engine %s\n", a.engine.c_str()); return 2; }
    std::filesystem::create_directories("runs");
    write_header_if_new(a.out);

    printf("== %s | %s | keys=%lld ops=%d runs=%d value=%dB scan_len=%d ==\n",
           a.engine.c_str(), a.mode.c_str(), a.keys, a.ops, a.runs, a.value_size, a.scan_len);

    vector<double> exec_samples;
    vector<double> slat_avg_samples, slat_p99_samples, wlat_p99_samples, ram_samples;

    for (int run = 0; run < a.runs; ++run) {
        RunResult r = (a.mode == "custom") ? run_custom(a, run) : run_ycsb(a, run);
        append_row(a.out, a, run, r);
        exec_samples.push_back(r.exec_ms);
        slat_avg_samples.push_back(r.slat.avg);
        slat_p99_samples.push_back(r.slat.p99);
        wlat_p99_samples.push_back(r.wlat.p99);
        ram_samples.push_back(r.ram_mb);

        if (a.mode == "custom") {
            double write_ms = r.rlat.avg, scan_ms = r.rlat.p50;
            printf("  run %d: write=%.0fms (%.0f ops/s) scan=%.0fms (%.0f ops/s) "
                   "slat_avg=%.4fms slat_p99=%.4fms ram=%.0fMB mig=%d build=%.1fms/%lldk\n",
                   run, write_ms, r.writes / (write_ms / 1000.0),
                   scan_ms, r.scans / (scan_ms / 1000.0),
                   r.slat.avg, r.slat.p99, r.ram_mb, r.ahse_migrations,
                   r.ahse_build_ms, r.ahse_build_keys / 1000);
        } else {
            printf("  run %d: exec=%.0fms slat_avg=%.4f slat_p99=%.4f rlat_avg=%.4f "
                   "ram=%.0fMB mig=%d build=%.1fms/%lldk\n",
                   run, r.exec_ms, r.slat.avg, r.slat.p99, r.rlat.avg,
                   r.ram_mb, r.ahse_migrations, r.ahse_build_ms, r.ahse_build_keys / 1000);
        }
    }

    auto ci_exec = metrics::mean_ci95(exec_samples);
    auto ci_slat = metrics::mean_ci95(slat_avg_samples);
    auto ci_slp99 = metrics::mean_ci95(slat_p99_samples);
    printf("  --- mean +/- 95%% CI over %d runs ---\n", a.runs);
    printf("  exec_ms     : %.1f +/- %.1f\n", ci_exec.mean, ci_exec.half_width);
    printf("  scan_avg_ms : %.5f +/- %.5f\n", ci_slat.mean, ci_slat.half_width);
    printf("  scan_p99_ms : %.5f +/- %.5f\n", ci_slp99.mean, ci_slp99.half_width);
    printf("  results appended to %s\n", a.out.c_str());
    return 0;
}
