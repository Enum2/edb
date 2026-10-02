# Honest Scan-Latency & HTAP Findings (Option A)

This note records the corrected, reviewer-defensible results after making every
range scan **fully materialize** the records it returns (Reviewer #3, O4). It
also documents why the submitted paper's 35–578× / 0.005 ms figures are not
honestly reproducible, and what the true, publishable claims are.

Hardware: Apple M-series (arm64), 14 cores, 48 GB RAM, macOS 26.5.1.
RocksDB 11.8.1, LMDB 1.0.1, Abseil. All engines share one RocksDB options
factory; all reads/scans materialize; identical operation sequences per run.

## 1. Why the 35–578× headline cannot be reproduced honestly

A range scan that reads its values must move `scan_len × record_size` bytes.
Once values are materialized, both engines become bandwidth-bound and the gap
between LMDB and RocksDB collapses. The only ways to obtain ~500× are:

1. **Not reading the scanned values** — walking the cursor/iterator and touching
   nothing. LMDB iteration then costs ~0 while RocksDB's multi-run merge still
   has overhead. This is precisely the defect Reviewer #3 flagged, and the most
   likely source of the original 0.005 ms / 578× numbers.
2. A pathologically un-compacted RocksDB. Real, but bounded — and materialization
   still caps the ratio.

Reproducing (1) would re-introduce the exact flaw that drew the reject. We do not.

## 2. Honest scan-latency landscape (RocksDB vs LMDB)

Scan latency is **workload-dependent**: large for short scans of small records
(iteration/merge-setup dominated) and small for long scans of large records
(data-movement dominated). Full grid, 2M keys, random load, materialized scans,
3 repetitions (average scan latency, ms):

| record size | scan length | RocksDB | LMDB | ratio |
|---:|---:|---:|---:|---:|
| 16 B   | 10   | 0.0054 | 0.0013 | 4.2× |
| 16 B   | 100  | 0.0149 | 0.0030 | 5.0× |
| 16 B   | 1000 | 0.1047 | 0.0190 | 5.5× |
| 100 B  | 10   | 0.0137 | 0.0021 | 6.5× |
| 100 B  | 100  | 0.0290 | 0.0111 | 2.6× |
| 100 B  | 1000 | 0.1924 | 0.1008 | 1.9× |
| 1 KB   | 10   | 0.0413 | 0.0121 | 3.4× |
| 1 KB   | 100  | 0.1591 | 0.1023 | 1.6× |
| 1 KB   | 1000 | 1.2984 | 1.0134 | 1.3× |

(Raw data: `sweep_results.csv`.) LMDB's floor of ~0.0013 ms matches — and beats —
the paper's "0.005 ms" claim honestly; the **RocksDB side is not milliseconds**
at these scales under normal compaction, so the honest per-scan ratio is
**1.3×–6.5×** (largest for short scans of small records), not 35–578×.

## 3. The real result: end-to-end HTAP (the workload the paper targets)

Alternating ingestion (random updates → compaction debt) and analytics (range
scans). 2M base keys, 3 cycles, 500k writes/cycle, 100k scans/cycle,
scan_len=200, 200 B records, 5 repetitions, mean ± 95% CI:

| engine | total (ms) | write (ms) | scan (ms) | scan lat (ms) |
|--------|-----------:|-----------:|----------:|--------------:|
| RocksDB       | 29,425 ± 1,068 | 4,022  | 25,403 | 0.085 |
| LMDB          | 27,270 ± 670   | 15,272 | 11,998 | 0.040 |
| AHA-inspired  | 29,576 ± 693   | 4,211  | 25,365 | 0.085 |
| **AHSE**      | **24,723 ± 1,805** | 7,589 | 17,134 | 0.057 |
| static_hybrid | 35,720 ± 3,080 | 23,480 | 12,240 | 0.041 |

Interpretation (all differences statistically significant, non-overlapping CIs):

- **RocksDB**: cheap writes, expensive scans.
- **LMDB**: cheap scans, expensive writes (single-writer mutex).
- **static_hybrid ("always maintain both")**: *worst* overall — the permanent
  dual-write tax makes ingestion dominate. This is the direct, quantitative
  answer to Reviewer #1 W4 / "why not always maintain both."
- **AHSE**: best end-to-end. It keeps near-RocksDB write cost and moves scan cost
  toward LMDB by migrating only during analytics phases, governed by the
  Q\*-Gate. **16% faster than RocksDB, 9% faster than LMDB, 31% faster than
  static-hybrid.**

- **AHA-inspired**: statistically identical to RocksDB (0.085 ms, 29.6 s). Under
  broad random analytical scans no single 1000-key segment gets hot enough to
  cross Eq. 1, so nothing morphs — the paper's own "incomplete acceleration,
  most segments stay cold" critique, now demonstrated. This is direct evidence
  for AHSE's full-index migration over AHA's segment-level adaptation.

AHSE's scan phase (17.1 s) is not as low as LMDB's (12.0 s) because migration is
reactive and the index is dropped when writes resume, so early scans in each
analytics phase run cold on RocksDB. This is an honest cost and points at the
predictive pre-build / hysteresis tuning discussed in future work.

## 4. Recommended claim changes for the resubmission

- Replace "35–578× lower scan latency" and "40.4× on Workload E" with the honest
  workload-dependent range (per-scan ~1.3–4× depending on selectivity/record
  size; end-to-end HTAP ~16% vs RocksDB, ~31% vs always-both).
- State explicitly in the methodology that scans fully materialize returned
  records (turns the R3 objection into a credibility point).
- Reframe the contribution around the **Q\*-Gate migration-decision model** and
  the **no-bad-quadrant** property (never as slow as LMDB on writes, never as
  slow as RocksDB on scans, and strictly better than always-both), not around a
  hero latency number.
- Keep static-hybrid in the evaluation as the evidentiary answer to W4.
