# Experiment 4 — Write Concurrency & Dual-Write Consistency

Addresses Reviewer #1 W3 / D7 / D8: concurrency and consistency were asserted in
prose (§3.5, §3.7) but never demonstrated. This experiment turns both into
measured evidence, and in doing so **found and fixed a real consistency hole**.

Setup: 2M base keys, 200 B records, N ∈ {1,4,8,16} concurrent writer threads
issuing random `put`s continuously. One migration is triggered mid-run
(`force_migration`, a test hook) so the migration transition window overlaps
live writes. Every write is timestamped and bucketed into migration-window vs
steady-state. After each run, a key-by-key **RocksDB (ground truth) vs LMDB**
diff is performed. 5 repetitions per writer count.

## Finding 1 — a real transition-window consistency bug (now fixed)

The submitted design enabled dual-write only after `is_lmdb_ready` flips true,
and built LMDB from a RocksDB **snapshot**. Writes that landed in the window
between *snapshot taken* and *flag flip* went to RocksDB only — so once LMDB
went live it served **stale** values. Measured on the unfixed path: **~100,000
stale keys per run** (≈ the number of window writes). This is precisely the
divergence Reviewer #2 (D7/D8) suspected.

**Fix** (`src/ahse/AHSEEngine.h`):
1. A `lmdb_writable_` flag opens the dual-write path immediately **after the
   index clear**, not only at `is_lmdb_ready` — so window writes are mirrored
   into LMDB with their fresh value.
2. The bulk copy uses `MDB_NOOVERWRITE`, so it never clobbers a fresher value a
   concurrent writer already wrote. Freshest write always wins, regardless of
   interleaving.

## Finding 2 — consistency is now provably maintained

| writers | runs | RocksDB↔LMDB divergence |
|--------:|-----:|:------------------------|
| 1  | 5 | 0 (0 missing, 0 mismatch) |
| 4  | 5 | 0 (0 missing, 0 mismatch) |
| 8  | 5 | 0 (0 missing, 0 mismatch) |
| 16 | 5 | 0 (0 missing, 0 mismatch) |

Across all 20 runs (2,000,000 keys each), LMDB matched RocksDB exactly after a
migration performed under sustained concurrent write load.

## Finding 3 — migration is non-disruptive to concurrent writers

Write p99 latency (ms), by bucket, mean over 5 reps (with the sequential-bulk-
load + buffered-catch-up migration path; see `optimization_mdb_append.md`):

| writers | steady-state p99 | migration-window p99 |
|--------:|-----------------:|---------------------:|
| 1  | 0.025 | 0.007 |
| 4  | 0.153 | 0.075 |
| 8  | 0.397 | 0.167 |
| 16 | 0.488 | 0.182 |

- **Steady-state** write p99 scales gently with concurrency (0.03 → 0.49 ms) —
  RocksDB + per-put LMDB dual-write under increasing contention.
- **Migration-window** write p99 is actually *lower* than steady-state, because
  during the build writers only append to RocksDB and record their key in an
  in-memory buffer — they do **not** contend with the bulk load on LMDB's writer
  mutex, and they skip the dual-write entirely for the duration of the build. The
  earlier design (writers dual-writing into LMDB during the build) produced a
  ~5–13 ms window spike; the buffered catch-up removes it.

Note: this reverses the paper's §7.1 claim that migration causes a P99 write
spike. With the improved migration path, **migration does not spike write
latency** — the modest write cost is the steady-state dual-write during the
read-heavy phase, not the migration episode itself.

## Takeaways for the paper

- Consistency is no longer an assertion: the transition-window hole is closed and
  verified at 0 divergence under 1–16 concurrent writers (§3.7 becomes evidence).
- The migration episode is non-disruptive: with the sequential bulk load +
  buffered catch-up, concurrent writers do not contend with the build, so there
  is no migration-window write spike. The remaining write cost is the
  steady-state dual-write during read-heavy phases (bounded, scales gently with
  concurrency), which honestly frames AHSE's applicability and motivates the
  future-work batched-dual-write / concurrent-B+-tree directions.
