# AHSE — Adaptive Hybrid Storage Engine

Reference implementation and benchmark suite for the paper *"AHSE: A Cost-Aware
Adaptive Hybrid Storage Engine for Mixed Analytical and Transactional
Workloads."*

AHSE keeps **RocksDB** (LSM-tree) as the durable primary store and provisions a
**LMDB** (memory-mapped B+-tree) secondary index on demand, governed by the
**AdaptiveBrain** cost model (AlphaCalibrator + Q\*-Gate + PhaseLearner).

# RUN CMD

SIZES="2000000" HTAP_SIZES="2000000 5000000 10000000" REPS=5 bash run_all_wsl.sh

## Layout

```
src/
  Engine.h              # common interface implemented by every engine
  Common.h              # canonical key format, cross-platform RSS, timing
  RocksDBConfig.h       # THE single RocksDB options factory (baseline == AHSE-internal)
  RocksDBEngine.h       # pure RocksDB baseline
  LMDBEngine.h          # standalone LMDB baseline (independent read-optimized baseline)
  AHATreeEngine.h       # AHA-inspired baseline (absl::btree_map, paper Eq. 1)
  StaticHybridEngine.h  # "always maintain both" strawman
  ahse/
    AdaptiveBrain.h     # AlphaCalibrator + QStarGate + PhaseLearner + state machine
    WorkloadMonitor.h   # background sampling + decision emission + telemetry
    TelemetryLogger.h   # extended per-tick CSV schema
    AHSEEngine.h        # the adaptive engine
  bench/
    Workload.h          # YCSB + custom workload generation
    Metrics.h           # percentiles + mean/95% CI
    EngineFactory.h     # construct engines by name with isolated data dirs
    PeakSampler.h       # per-run peak-RSS sampler
    main.cpp            # unified driver (ahse_bench): YCSB + custom sweep
    exp1_workload_a.cpp # Experiment 1: Workload-A anomaly (H1/H2) driver
    htap.cpp            # HTAP driver: alternating ingestion/analytics
    exp4_concurrency.cpp# Experiment 4: concurrency + dual-write consistency
CMakeLists.txt
run_all.sh              # process-isolated multi-run wrapper (YCSB / custom)
run_sweep.sh            # scan-latency sweep: RocksDB vs LMDB over (record x scan len)
run_htap.sh             # end-to-end HTAP comparison across all engines
docs/                   # experiment write-ups (root-cause note, honest findings)
```

The original submission files (`StorageManager.h`, `adaptive_brain.h`,
`WorkloadMonitor.h`, `TelemetryLogger.h`, `AHATree.h`, `benchmark_*.cpp`,
`ycsb_benchmark.cpp`) are retained at the repository root for reference. The
`src/` tree is the corrected, buildable implementation.

## Dependencies

- C++20 compiler (Apple clang or GCC ≥ 10)
- CMake ≥ 3.16
- RocksDB, LMDB, Abseil

### macOS (Homebrew)

```bash
brew install cmake rocksdb lmdb abseil
```

### Linux (Debian/Ubuntu)

```bash
sudo apt-get install cmake librocksdb-dev liblmdb-dev libabsl-dev
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

If dependencies live outside the default prefix, pass `-DDEP_PREFIX=/path`.

## Run

Single configuration, 5 in-process repetitions with mean ± 95% CI:

```bash
./build/ahse_bench --engine ahse --mode ycsb-e --keys 1000000 --ops 200000 \
                   --runs 5 --scan-len 1000 --fixed-scan --out results.csv
```

Process-isolated runs (fresh process per repetition; clean peak-RAM):

```bash
MODE=ycsb-e KEYS=1000000 REPS=5 ./run_all.sh
```

### Options

| flag | meaning |
|------|---------|
| `--engine` | `rocksdb`, `lmdb`, `aha`, `static_hybrid`, `ahse` |
| `--mode` | `ycsb-a` (50/50), `ycsb-b` (95/5), `ycsb-e` (95% scan), `custom` |
| `--keys` | dataset size (records loaded) |
| `--ops` | operations (YCSB) or scans (custom scan phase) |
| `--runs` | in-process repetitions |
| `--scan-len` | scan range length |
| `--fixed-scan` | every scan spans exactly `scan-len` (paper's Workload E) |
| `--out` | results CSV (one row per repetition) |
| `--tag` | data-directory / telemetry prefix |
| `--verbose` | print brain decisions to stderr |

## Outputs

- **`results*.csv`** — one row per repetition: exec time, per-op-type latency
  percentiles, peak RAM, and AHSE build/migration internals.
- **`runs/<tag>_telemetry.csv`** — AHSE per-tick control log: phase,
  `successful_migrations`, all control flags, `is_lmdb_ready`, `is_migrating`,
  `Q_expected`, `Q*`, `C_build`, scan-rate EMA (Experiment 1 / 4 instrumentation).

## Hardware note

Numbers are only comparable within one machine. Record the machine used; do not
mix results across different hardware.

## Experiments

All scans fully materialize the records they return, so latencies are honest
(bandwidth/merge-bound), not artifacts of skipping value reads.

```bash
# Experiment 1 — Workload-A anomaly root cause (clean vs leaky, H1/H2)
./build/exp1_workload_a --keys 1000000 --ops 200000 --reps 5 --out exp1_leaky.csv

# Scan-latency landscape — RocksDB vs LMDB over (record size x scan length)
./run_sweep.sh                      # -> sweep_results.csv

# End-to-end HTAP — all engines, alternating ingestion/analytics
./run_htap.sh                       # -> htap_results.csv

# Experiment 4 — write concurrency + RocksDB/LMDB consistency diff
./build/exp4_concurrency --keys 2000000 --value-size 200 --reps 5 --out exp4_results.csv
```

Write-ups with the measured tables and the recommended paper-claim revisions are
in `docs/experiment1_root_cause.md` and `docs/honest_scan_and_htap_findings.md`.
