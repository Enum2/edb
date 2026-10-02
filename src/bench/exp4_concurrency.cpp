//
// exp4_concurrency.cpp — Experiment 4: write concurrency + dual-write consistency.
//
// Addresses Reviewer #1 W3 / D7 / D8 (concurrency & consistency claimed only in
// prose). Under N concurrent writer threads we:
//   * deterministically trigger ONE migration mid-run (force_migration) while
//     writes continue, so the migration transition window overlaps live writes;
//   * timestamp every write and bucket its latency into "migration-window" vs
//     "steady-state" (window = [snapshot taken, is_lmdb_ready flip]);
//   * after the run, perform a key-by-key RocksDB (ground truth) vs LMDB diff to
//     detect any staleness/loss introduced by concurrent writes during the
//     transition window.
//
// This is a controlled experiment: force_migration() is a test hook so the
// migration window is deterministic rather than dependent on workload timing.
//
#include "Metrics.h"
#include "../Common.h"
#include "../ahse/AHSEEngine.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <random>
#include <chrono>

using namespace std;

struct Sample { long long ts_ns; double lat_ms; };

int main(int argc, char** argv) {
    long long keys = 2000000;
    int value_size = 200;
    int reps = 5;
    vector<int> writers = {1, 4, 8, 16};
    string out = "exp4_results.csv";
    for (int i = 1; i < argc; ++i) {
        string s = argv[i];
        auto nv = [&]{ return string(argv[++i]); };
        if (s == "--keys") keys = stoll(nv());
        else if (s == "--value-size") value_size = stoi(nv());
        else if (s == "--reps") reps = stoi(nv());
        else if (s == "--out") out = nv();
    }

    std::filesystem::create_directories("runs");
    bool newf = !std::filesystem::exists(out);
    ofstream f(out, std::ios::app);
    if (newf)
        f << "writers,rep,total_writes,mig_writes,steady_writes,"
             "mig_p50,mig_p95,mig_p99,mig_p999,steady_p50,steady_p95,steady_p99,steady_p999,"
             "cons_total,cons_missing,cons_mismatch,build_ms,window_ms\n";

    printf("== Experiment 4: concurrency + consistency | keys=%lld val=%dB reps=%d ==\n",
           keys, value_size, reps);

    for (int W : writers) {
        for (int rep = 0; rep < reps; ++rep) {
            string tag = "exp4_w" + to_string(W) + "_r" + to_string(rep);
            AHSEEngine engine(keys,
                              "runs/" + tag + "_rocks",
                              "runs/" + tag + "_lmdb",
                              "runs/" + tag + "_telemetry.csv");
            // Base load (untimed), sequential is fine here — the focus is the
            // concurrent-write / migration interaction, not scan cost.
            string v = kf::make_value(value_size);
            for (long long i = 0; i < keys; ++i) engine.put(kf::make_key(i), v);
            engine.on_load_complete();

            std::atomic<bool> stop{false};
            std::atomic<bool> go{false};
            vector<vector<Sample>> per_thread(W);

            auto writer_fn = [&](int tid) {
                std::mt19937_64 rng(7000 + tid + rep * 131);
                std::uniform_int_distribution<long long> kd(0, keys - 1);
                // DISTINCT value ('W' fill) so an update that fails to propagate
                // to LMDB is detectable as a mismatch against RocksDB ('v' base).
                string wv(value_size, 'W');
                vector<Sample>& out_s = per_thread[tid];
                out_s.reserve(2000000);
                while (!go.load()) std::this_thread::yield();
                while (!stop.load()) {
                    long long k = kd(rng);
                    long long t0 = AHSEEngine::now_ns();
                    engine.put(kf::make_key(k), wv);
                    long long t1 = AHSEEngine::now_ns();
                    out_s.push_back({t0, (t1 - t0) / 1e6});
                }
            };

            vector<thread> pool;
            for (int t = 0; t < W; ++t) pool.emplace_back(writer_fn, t);
            go.store(true);

            // Steady state, then force a migration mid-run while writers hammer.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            engine.force_migration();
            // Wait for the migration to complete (bounded).
            auto wstart = std::chrono::steady_clock::now();
            while (!engine.is_lmdb_active() &&
                   std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - wstart).count() < 30)
                std::this_thread::yield();
            // Continue steady-state (now dual-writing) briefly.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            stop.store(true);
            for (auto& t : pool) t.join();

            long long bstart = engine.build_start_ns();
            long long bend = engine.build_end_ns();
            double build_ms = engine.last_build_ms();

            vector<double> mig_lat, steady_lat;
            long long total = 0;
            for (auto& vec : per_thread) {
                for (auto& s : vec) {
                    total++;
                    if (s.ts_ns >= bstart && s.ts_ns <= bend) mig_lat.push_back(s.lat_ms);
                    else steady_lat.push_back(s.lat_ms);
                }
            }
            metrics::LatencyStats ms = metrics::compute(mig_lat);
            metrics::LatencyStats ss = metrics::compute(steady_lat);

            auto cons = engine.verify_consistency();

            f << W << ',' << rep << ',' << total << ',' << mig_lat.size() << ','
              << steady_lat.size() << ','
              << ms.p50 << ',' << ms.p95 << ',' << ms.p99 << ',' << ms.p999 << ','
              << ss.p50 << ',' << ss.p95 << ',' << ss.p99 << ',' << ss.p999 << ','
              << cons.total << ',' << cons.missing << ',' << cons.mismatch << ','
              << build_ms << ',' << (bend - bstart) / 1e6 << '\n' << flush;

            printf("  W=%2d rep %d: writes=%lld (mig_win=%zu) steady_p99=%.4f mig_p99=%.4f "
                   "build=%.0fms window=%.0fms | consistency: total=%lld missing=%lld mismatch=%lld\n",
                   W, rep, total, mig_lat.size(), ss.p99, ms.p99, build_ms,
                   (bend - bstart) / 1e6, cons.total, cons.missing, cons.mismatch);
        }
    }
    printf("  -> %s\n", out.c_str());
    return 0;
}
