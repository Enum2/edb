# Experiment 1 — Workload A Anomaly: Root-Cause Note

**Question.** The submitted paper reports AHSE **28.2% faster** than RocksDB on
YCSB Workload A at 10M records (22,959 ms vs 31,996 ms), even though Workload A
has read ratio *r* = 0.5, below the read-heavy threshold (*r* > 0.80) that gates
migration. If migration never fired, an engine that embeds the same RocksDB
should not beat RocksDB. This note tests the two hypotheses (H1 state leakage,
H2 configuration/measurement mismatch) on the corrected, process-isolated
harness.

**Hardware.** Apple M-series (arm64), 14 cores, 48 GB RAM, macOS 26.5.1.
RocksDB 11.8.1, LMDB 1.0.1, Abseil 20260817. All numbers regenerated on this one
machine; they are not comparable to the original Fedora/16 GB figures and are not
meant to be — Table 1 is being regenerated in full.

## Method

Three configurations, fresh data directories, 3–5 repetitions, mean ± 95% CI
(Student-*t*):

- **CLEAN_ROCKS** — standalone RocksDB, Workload A. Own process.
- **CLEAN_AHSE** — fresh AHSE, Workload A only. Own process.
- **LEAKY_AHSE** — fresh AHSE, a read-heavy warmup that forces migration, then
  Workload A on the *same* engine with no reset (reproduces H1).

Per-tick telemetry (`runs/*_telemetry.csv`) records phase,
`successful_migrations`, all control flags, `is_lmdb_ready`, `is_migrating`.

## Result 1 — migration never fires under Workload A (any scale)

Process-isolated Workload A, one process per run:

| scale | RocksDB (ms) | AHSE (ms) | AHSE migrations | LMDB ever active |
|------:|-------------:|----------:|:---------------:|:----------------:|
| 1M  | 399.2 ± 25.2  | 385.2 ± 29.3  | 0 | no |
| 5M  | 1308.1 ± 31.8 | 1499.5 ± 48.1 | 0 | no |
| 10M | 1420.3 ± 125.2| 1413.4 ± 96.0 | 0 | no |

In **every** AHSE run at every scale, `successful_migrations = 0` and
`is_lmdb_ready` never became true. The Q\*-Gate / read-heavy gating behaves
exactly as specified: at *r* = 0.5 no migration is triggered. At 1M and 10M the
confidence intervals overlap (AHSE ≈ RocksDB); at 5M AHSE is ~15% **slower**,
reflecting the monitor/flag-check overhead on the write-heavy path. **AHSE is
never significantly faster than RocksDB on Workload A.** The reported 28.2%
speedup does not reproduce.

## Result 2 — H2 (configuration) ruled out; measurement *ordering* is the artifact

The standalone baseline and AHSE's internal RocksDB are constructed by the *same*
factory (`ahse::rocksdb_options()`), so their options are identical by
construction:

```
compaction_style=0 (Level)  write_buffer_size=67108864  max_write_buffer_number=2
level0_file_num_compaction_trigger=4  max_background_jobs=2  compression=Snappy
```

There is no config-struct mismatch. However, the *measurement setup* is the real
culprit. The submitted `ycsb_benchmark.cpp` runs the baseline (TEST 1) and the
adaptive engine (TEST 2) **sequentially in one process**. Whichever engine runs
second inherits a warmed allocator, warm filesystem/page cache, and elevated CPU
clocks. When both configs share a process, CLEAN_AHSE (run second) measured
~10% faster than CLEAN_ROCKS *with zero migration*; once each engine is put in
its **own process** (Result 1), that advantage disappears. The 28.2% figure is
consistent with this run-ordering/warmup bias, not with any AHSE mechanism.

## Result 3 — H1 (state leakage) is a real hazard, but cannot create a *speedup*

LEAKY_AHSE (1M, in-process): a read-heavy warmup migrates the engine, then
Workload A runs on the same engine without reset.

| config | exec (ms) | migrations | LMDB active during A |
|--------|----------:|:----------:|:--------------------:|
| CLEAN_ROCKS | 354.9 ± 16.3 | 0 | no |
| CLEAN_AHSE  | 359.5 ± 24.0 | 0 | no |
| LEAKY_AHSE  | **1294.5 ± 36.8** | 15 | yes (5/5) |

Leaked state makes Workload A run on LMDB with dual-write, which is **~3.6×
slower** (the single-writer-mutex dual-write tax on the 50% updates), not faster.
So state leakage is a genuine correctness hazard that would corrupt results, but
it produces a *slowdown* — it cannot explain the reported speedup.

## Conclusion

- Migration provably does **not** fire under Workload A (*r* = 0.5); the gating
  logic is correct. Corrected Table 1 Workload A: **AHSE ≈ RocksDB** (marginally
  slower at 5M from monitoring overhead), *not* 28.2% faster.
- The anomaly was a **measurement-ordering / warmup artifact** of running both
  engines in one process (a measurement-setup instance of H2), fixed by
  process-per-run isolation. H2-as-config-mismatch is ruled out.
- H1 leakage is real and now prevented by the isolated harness, but it slows A
  down rather than speeding it up.

## Checkpoint answer (required before Experiments 2–4)

**Yes — Workloads B, E, and the full custom 7-size benchmark must be re-run under
the process-isolated harness before any Table 1 / Figure is treated as final.**
The ordering/warmup bias is a property of the *harness*, not of Workload A, so it
inflated B and E magnitudes too (there the effect hides because B/E are supposed
to win). `run_all.sh` now enforces one process per engine per repetition with
wiped data directories, and every run emits raw per-run CSV plus AHSE telemetry.
