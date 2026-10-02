//
// exp1_workload_a.cpp — Experiment 1: Workload-A anomaly root cause.
//
// The submitted paper reported AHSE 28.2% FASTER than RocksDB on YCSB Workload A
// at 10M, despite Workload A's read ratio (r=0.5) being below the read-heavy
// threshold (r>0.80) that gates migration. This driver tests the two hypotheses:
//
//   H1 (state leakage): a prior read-heavy run left the engine migrated
//       (is_lmdb_ready = true, successful_migrations >= 1); that state carried
//       into Workload A, so A silently ran on LMDB + dual-write.
//   H2 (config mismatch): the standalone baseline RocksDB and AHSE's internal
//       RocksDB used different options / measurement setups.
//
// It runs three configurations per repetition, all on ONE machine, fresh dirs:
//   CLEAN_ROCKS : standalone RocksDB, Workload A.
//   CLEAN_AHSE  : fresh AHSE, Workload A only (correct/isolated behavior).
//   LEAKY_AHSE  : fresh AHSE, a read-heavy warmup that forces migration, THEN
//                 Workload A on the SAME engine WITHOUT reset (reproduces H1).
//
// For each it records exec time, whether LMDB was active during A, and the
// migration counter. Per-tick telemetry is written per configuration.
//
#include <cstdint>
#include "Workload.h"
#include "Metrics.h"
#include "EngineFactory.h"
#include "PeakSampler.h"
#include "../Common.h"
#include "../RocksDBEngine.h"
#include "../ahse/AHSEEngine.h"
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>

using namespace std;

static double run_ops(Engine& e, const vector<wl::Op>& ops,
                      uint64_t* reads, uint64_t* writes, uint64_t* scans) {
    auto t0 = timing::clock::now();
    for (const auto& op : ops) {
        switch (op.type) {
            case wl::OpType::READ:   e.get(op.key); (*reads)++; break;
            case wl::OpType::SCAN:   e.scan(op.key, op.key_end); (*scans)++; break;
            case wl::OpType::UPDATE:
            case wl::OpType::INSERT: e.put(op.key, op.value); (*writes)++; break;
            case wl::OpType::RMW:    e.get(op.key); e.put(op.key, op.value); (*reads)++; (*writes)++; break;
        }
    }
    return timing::ms_since(t0);
}

static wl::YcsbConfig cfg_A(long long keys, int ops) {
    wl::YcsbConfig c; c.num_ops = ops; c.num_keys = keys;
    c.read_pct = 50; c.update_pct = 50; c.scan_pct = 0; c.insert_pct = 0;
    return c;
}
static wl::YcsbConfig cfg_E(long long keys, int ops) {
    wl::YcsbConfig c; c.num_ops = ops; c.num_keys = keys;
    c.read_pct = 0; c.update_pct = 0; c.scan_pct = 95; c.insert_pct = 5;
    c.scan_len = 1000; c.fixed_scan = true;
    return c;
}

int main(int argc, char** argv) {
    long long keys = 1000000;
    int ops = 200000;
    int reps = 5;
    string out = "exp1_results.csv";
    for (int i = 1; i < argc; ++i) {
        string s = argv[i];
        auto nv = [&]{ return string(argv[++i]); };
        if (s == "--keys") keys = stoll(nv());
        else if (s == "--ops") ops = stoi(nv());
        else if (s == "--reps") reps = stoi(nv());
        else if (s == "--out") out = nv();
    }

    std::filesystem::create_directories("runs");
    bool new_file = !std::filesystem::exists(out);
    std::ofstream f(out, std::ios::app);
    if (new_file)
        f << "keys,rep,config,exec_ms,reads,writes,scans,migrations,"
             "lmdb_active_after,peak_ram_mb,checksum\n";

    printf("== Experiment 1: Workload A anomaly | keys=%lld ops=%d reps=%d ==\n",
           keys, ops, reps);

    // ---- H2 config artifact: baseline and AHSE-internal RocksDB come from the
    // SAME factory (ahse::rocksdb_options), so their options are identical by
    // construction. Dump the key fields the reviewer named. ----
    {
        rocksdb::Options o = ahse::rocksdb_options();
        printf("  [H2] RocksDB options (identical for baseline & AHSE-internal):\n");
        printf("       compaction_style=%d write_buffer_size=%zu max_write_buffer_number=%d\n",
               (int)o.compaction_style, (size_t)o.write_buffer_size, o.max_write_buffer_number);
        printf("       level0_file_num_compaction_trigger=%d max_background_jobs=%d compression=%d\n",
               o.level0_file_num_compaction_trigger, o.max_background_jobs, (int)o.compression);
        printf("       create_if_missing=%d  (source: ahse::rocksdb_options() — one factory)\n",
               (int)o.create_if_missing);
    }

    for (int rep = 0; rep < reps; ++rep) {
        // ---- CLEAN_ROCKS ----
        {
            string tag = "exp1_rocks_" + to_string(keys) + "_" + to_string(rep);
            RocksDBEngine e(bench::data_dir(tag, "rocks"));
            PeakSampler peak;
            for (long long i = 0; i < keys; ++i) e.put(kf::make_key(i), "initial_ycsb_payload_data__");
            auto A = wl::generate_ycsb(cfg_A(keys, ops));
            uint64_t rd=0, wr=0, sc=0;
            double ms = run_ops(e, A, &rd, &wr, &sc);
            peak.stop();
            f << keys << ',' << rep << ",CLEAN_ROCKS," << ms << ',' << rd << ',' << wr << ','
              << sc << ",0,0," << peak.peak_mb() << ',' << e.checksum() << '\n' << flush;
            printf("  rep %d CLEAN_ROCKS : exec=%.0fms\n", rep, ms);
        }
        // ---- CLEAN_AHSE ----
        {
            string tag = "exp1_ahse_clean_" + to_string(keys) + "_" + to_string(rep);
            AHSEEngine e(keys, bench::data_dir(tag, "rocks"), bench::data_dir(tag, "lmdb"),
                         "runs/" + tag + "_telemetry.csv");
            PeakSampler peak;
            for (long long i = 0; i < keys; ++i) e.put(kf::make_key(i), "initial_ycsb_payload_data__");
            auto A = wl::generate_ycsb(cfg_A(keys, ops));
            uint64_t rd=0, wr=0, sc=0;
            double ms = run_ops(e, A, &rd, &wr, &sc);
            peak.stop();
            int mig = e.monitor().brain.successful_migrations();
            bool active = e.is_lmdb_active();
            f << keys << ',' << rep << ",CLEAN_AHSE," << ms << ',' << rd << ',' << wr << ','
              << sc << ',' << mig << ',' << (active?1:0) << ',' << peak.peak_mb() << ','
              << e.checksum() << '\n' << flush;
            printf("  rep %d CLEAN_AHSE  : exec=%.0fms mig=%d lmdb_active_after=%d\n",
                   rep, ms, mig, active);
        }
        // ---- LEAKY_AHSE (reproduce H1) ----
        {
            string tag = "exp1_ahse_leaky_" + to_string(keys) + "_" + to_string(rep);
            AHSEEngine e(keys, bench::data_dir(tag, "rocks"), bench::data_dir(tag, "lmdb"),
                         "runs/" + tag + "_telemetry.csv");
            PeakSampler peak;
            for (long long i = 0; i < keys; ++i) e.put(kf::make_key(i), "initial_ycsb_payload_data__");
            // Read-heavy warmup: drive scans until migration completes (or timeout).
            auto warm = wl::generate_ycsb(cfg_E(keys, ops));
            auto tstart = std::chrono::steady_clock::now();
            size_t idx = 0;
            while (!e.is_lmdb_active() &&
                   std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - tstart).count() < 15) {
                const auto& op = warm[idx % warm.size()];
                e.scan(op.key, op.key_end);
                if (++idx % 2000 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            bool migrated = e.is_lmdb_active();
            // Now run Workload A on the SAME engine WITHOUT reset — leaked state.
            auto A = wl::generate_ycsb(cfg_A(keys, ops));
            uint64_t rd=0, wr=0, sc=0;
            double ms = run_ops(e, A, &rd, &wr, &sc);
            peak.stop();
            int mig = e.monitor().brain.successful_migrations();
            bool active_after = e.is_lmdb_active();
            f << keys << ',' << rep << ",LEAKY_AHSE," << ms << ',' << rd << ',' << wr << ','
              << sc << ',' << mig << ',' << (active_after?1:0) << ',' << peak.peak_mb() << ','
              << e.checksum() << '\n' << flush;
            printf("  rep %d LEAKY_AHSE  : warmup_migrated=%d exec=%.0fms mig=%d lmdb_active_during_A=%d\n",
                   rep, migrated, ms, mig, active_after);
        }
    }

    // Aggregate: mean exec per config.
    printf("  results -> %s\n", out.c_str());
    return 0;
}
