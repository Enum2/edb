//
// htap.cpp — realistic HTAP (alternating ingestion / analytics) benchmark.
//
// This is the workload AHSE actually targets: phases of write-heavy ingestion
// alternate with phases of read-heavy range-scan analytics. During an ingestion
// burst RocksDB accumulates compaction debt (many overlapping SSTable runs), so
// the following analytics phase pays real multi-run merge cost on every scan.
// AHSE detects the read-heavy phase and (if the Q*-Gate approves) migrates scans
// onto the LMDB B+-tree; when writes resume it drops back. LMDB-only and
// static-hybrid are included as the read-optimized and "always both" references.
//
// Correctness / fairness properties (for reviewer scrutiny):
//   * every engine replays the IDENTICAL operation sequence (RNG seeded only by
//     repetition index, not by engine), so comparisons are apples-to-apples;
//   * all reads/scans fully materialize their values (Engine::materialize);
//   * base load order is randomized (realistic ingestion, not pre-sorted);
//   * no artificial sleeps; phases are sized so the 500ms monitor windows can
//     observe genuine phase transitions;
//   * 5 repetitions, mean +/- 95% CI reported.
//
#include "Workload.h"
#include "Metrics.h"
#include "EngineFactory.h"
#include "PeakSampler.h"
#include "../Common.h"
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <numeric>
#include <random>
#include <algorithm>

using namespace std;

struct Args {
    string engine = "ahse";
    long long keys = 5000000;
    int cycles = 3;
    long long writes_per_cycle = 1000000;
    int scans_per_cycle = 50000;
    int scan_len = 100;
    int value_size = 100;
    int runs = 5;
    bool warmup = false;          // prime cache after base load, before timing
    int read_phase_write_pct = 0; // item 4: inject P% writes into the analytics
                                  // burst to soften the read-heavy phase
                                  // (0 = pure-scan phase, the original behavior)
    string out = "htap_results.csv";
    bool verbose = false;
};

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        string s = argv[i];
        auto nv = [&]{ return string(argv[++i]); };
        if (s == "--engine") a.engine = nv();
        else if (s == "--keys") a.keys = stoll(nv());
        else if (s == "--cycles") a.cycles = stoi(nv());
        else if (s == "--writes-per-cycle") a.writes_per_cycle = stoll(nv());
        else if (s == "--scans-per-cycle") a.scans_per_cycle = stoi(nv());
        else if (s == "--scan-len") a.scan_len = stoi(nv());
        else if (s == "--value-size") a.value_size = stoi(nv());
        else if (s == "--runs") a.runs = stoi(nv());
        else if (s == "--read-phase-write-pct") a.read_phase_write_pct = stoi(nv());
        else if (s == "--warmup") a.warmup = true;
        else if (s == "--out") a.out = nv();
        else if (s == "--verbose") a.verbose = true;
        else { fprintf(stderr, "unknown arg %s\n", s.c_str()); exit(2); }
    }
    return a;
}

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    if (!bench::engine_exists(a.engine)) { fprintf(stderr, "unknown engine\n"); return 2; }
    std::filesystem::create_directories("runs");
    bool new_file = !std::filesystem::exists(a.out);
    std::ofstream f(a.out, std::ios::app);
    if (new_file)
        f << "engine,keys,cycles,writes_per_cycle,scans_per_cycle,scan_len,value_size,run,"
             "total_ms,write_ms,scan_ms,wlat_avg,wlat_p99,slat_avg,slat_p50,slat_p95,slat_p99,slat_p999,"
             "peak_ram_mb,gate_votes,phys_migrations,build_ms,build_keys,checksum\n";

    printf("== HTAP | %s | keys=%lld cycles=%d W/cyc=%lld S/cyc=%d len=%d val=%dB runs=%d rpw=%d%% ==\n",
           a.engine.c_str(), a.keys, a.cycles, a.writes_per_cycle, a.scans_per_cycle,
           a.scan_len, a.value_size, a.runs, a.read_phase_write_pct);

    vector<double> slat_avg_s, slat_p99_s, scan_ms_s, total_s;

    for (int rep = 0; rep < a.runs; ++rep) {
        string tag = "htap_" + a.engine + "_" + to_string(a.keys) + "_" + to_string(rep);
        string tel = "runs/" + tag + "_telemetry.csv";
        auto engine = bench::make_engine(a.engine, a.keys, tag, tel, a.verbose, a.value_size);
        PeakSampler peak;

        // Base load, randomized order (untimed).
        {
            string v = kf::make_value(a.value_size);
            vector<long long> order(a.keys);
            std::iota(order.begin(), order.end(), 0LL);
            std::mt19937_64 lrng(999 + rep);
            std::shuffle(order.begin(), order.end(), lrng);
            for (long long k : order) engine->put(kf::make_key(k), v);
            engine->on_load_complete();
            if (a.warmup) engine->prewarm();   // prime cache before timed cycles
        }

        // Deterministic per-rep RNG so every engine replays the same sequence.
        std::mt19937_64 rng(1000 + rep);
        std::uniform_int_distribution<long long> keydist(0, a.keys - 1);
        std::uniform_int_distribution<long long> scandist(0, a.keys - a.scan_len - 1);
        string wval = kf::make_value(a.value_size);

        vector<double> wlat; wlat.reserve(a.writes_per_cycle * a.cycles);
        vector<double> slat; slat.reserve((size_t)a.scans_per_cycle * a.cycles);
        double write_ms = 0, scan_ms = 0;
        auto t_all = timing::clock::now();

        for (int c = 0; c < a.cycles; ++c) {
            // --- ingestion burst (creates compaction debt) ---
            auto tw = timing::clock::now();
            for (long long i = 0; i < a.writes_per_cycle; ++i) {
                long long k = keydist(rng);
                auto s = timing::clock::now();
                engine->put(kf::make_key(k), wval);
                wlat.push_back(timing::ms_since(s));
            }
            write_ms += timing::ms_since(tw);

            // --- analytics burst (range scans under compaction debt) ---
            // When read_phase_write_pct > 0, a deterministic fraction of the
            // burst is writes instead of scans, softening the read-heavy phase
            // (item 4: phase-skew sweep). The RNG is per-rep and engine-agnostic,
            // so every engine still replays the IDENTICAL operation sequence.
            auto ts = timing::clock::now();
            for (int i = 0; i < a.scans_per_cycle; ++i) {
                if (a.read_phase_write_pct > 0 &&
                    (int)(rng() % 100) < a.read_phase_write_pct) {
                    long long wk = keydist(rng);
                    auto sw = timing::clock::now();
                    engine->put(kf::make_key(wk), wval);
                    wlat.push_back(timing::ms_since(sw));
                }
                long long k = scandist(rng);
                auto s = timing::clock::now();
                engine->scan(kf::make_key(k), kf::make_key(k + a.scan_len));
                slat.push_back(timing::ms_since(s));
            }
            scan_ms += timing::ms_since(ts);
        }

        double total_ms = timing::ms_since(t_all);
        peak.stop();

        metrics::LatencyStats ws = metrics::compute(wlat);
        metrics::LatencyStats ss = metrics::compute(slat);

        long long build_keys = 0; double build_ms = 0; int migs = 0; int phys = 0;
        if (auto* ah = dynamic_cast<AHSEEngine*>(engine.get())) {
            build_keys = ah->last_build_keys();
            build_ms = ah->last_build_ms();
            migs = ah->gate_approvals();        // per-tick vote tally
            phys = ah->physical_migrations();   // completed build windows
        }

        f << a.engine << ',' << a.keys << ',' << a.cycles << ',' << a.writes_per_cycle << ','
          << a.scans_per_cycle << ',' << a.scan_len << ',' << a.value_size << ',' << rep << ','
          << total_ms << ',' << write_ms << ',' << scan_ms << ','
          << ws.avg << ',' << ws.p99 << ',' << ss.avg << ',' << ss.p50 << ',' << ss.p95 << ','
          << ss.p99 << ',' << ss.p999 << ',' << peak.peak_mb() << ',' << migs << ',' << phys << ','
          << build_ms << ',' << build_keys << ',' << engine->checksum() << '\n' << flush;

        printf("  run %d: total=%.0fms write=%.0fms scan=%.0fms slat_avg=%.4f slat_p99=%.4f "
               "ram=%.0fMB votes=%d phys_mig=%d build=%.0fms/%lldk\n",
               rep, total_ms, write_ms, scan_ms, ss.avg, ss.p99, peak.peak_mb(),
               migs, phys, build_ms, build_keys / 1000);

        slat_avg_s.push_back(ss.avg); slat_p99_s.push_back(ss.p99);
        scan_ms_s.push_back(scan_ms); total_s.push_back(total_ms);
    }

    auto ci_savg = metrics::mean_ci95(slat_avg_s);
    auto ci_sp99 = metrics::mean_ci95(slat_p99_s);
    auto ci_scan = metrics::mean_ci95(scan_ms_s);
    printf("  --- mean +/- 95%% CI over %d runs ---\n", a.runs);
    printf("  scan_phase_ms : %.1f +/- %.1f\n", ci_scan.mean, ci_scan.half_width);
    printf("  scan_avg_ms   : %.5f +/- %.5f\n", ci_savg.mean, ci_savg.half_width);
    printf("  scan_p99_ms   : %.5f +/- %.5f\n", ci_sp99.mean, ci_sp99.half_width);
    printf("  -> %s\n", a.out.c_str());
    return 0;
}
