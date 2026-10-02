# Experiment 2a — AHA-tree Baseline: Availability & Fidelity Disclosure

## Availability decision (time-boxed, per plan)

A public reference implementation of the AHA-tree (Xing & Aref) **does not
exist**. The two sources are both Purdue/Aref papers with no linked code
artifact:

- *The AHA-Tree: An Adaptive Index for HTAP Workloads* (demo), arXiv:2406.08746.
- *Investigation of Adaptive Hotspot-Aware Indexes for Oscillating Write-Heavy
  and Read-Heavy Workloads — An Experimental Study*, arXiv:2406.09372 /
  cs.purdue.edu/homes/aref/CGR/ahatree2024.pdf.

Neither provides source, a GitHub link, or a reproducibility artifact. We
therefore take the plan's fallback path (2a-b): retain an **AHA-inspired**
baseline and document exactly what it does and does not capture.

## What the real AHA-tree does (from the papers)

- A single index that **morphs bi-directionally** between a pure LSM-tree
  (write-optimized) and a pure buffered B-tree (read-optimized), with meaningful
  **intermediate states** in between.
- Adaptation is **online and non-blocking** (zero downtime, concurrent
  operations continue during morphing).
- Because real data is skewed, adaptation is **hotspot-aware**: it optimizes the
  index within hot key regions rather than the whole keyspace.
- After a transient adaptation period it matches B+-tree read performance, and it
  stays competitive with an LSM-tree on write-heavy phases.

## What our baseline (`src/AHATreeEngine.h`) captures

- Keyspace partitioned into fixed **1000-key segments** (hotspot granularity).
- A segment migrates **COLD_LSM → HOT_BPLUS** exactly per the paper's Eq. 1:
  `R/(R+W) > 0.7 AND R*0.04 > 1000*0.02`, evaluated on per-segment read/write
  counts in a background monitor.
- Hot segments serve reads/scans from an in-memory `absl::btree_map` and
  dual-write to RocksDB for consistency; cold segments stay in RocksDB.

## What our baseline does NOT capture (honest limitations)

1. **Unidirectional only.** Ours migrates COLD_LSM → HOT_BPLUS and never morphs
   back; the real AHA-tree adapts in **both** directions.
2. **Binary state, no morphing continuum.** Ours is either LSM or an in-memory
   B-tree per segment. The real AHA-tree has a **buffered B-tree with
   intermediate states**; we do not model the buffer-tree structure or partial
   states.
3. **Blocking morph.** Our morph takes a per-segment exclusive lock while
   copying; the real AHA-tree morphs **without downtime** under concurrency.
4. **In-memory, non-persistent hot index.** Our hot structure is a transient
   `absl::btree_map`, not a persistent, crash-recoverable index.
5. **Simpler hotspot detection.** Fixed 1000-key segments with a threshold rule,
   versus the paper's more general hotspot-region treatment.

## How this is reported in the paper

The baseline is labeled "AHA-inspired" and the limitations above are stated
explicitly, so no claim is made that we reproduce the full AHA-tree. This is the
honest framing Reviewers #2 (D3/O3/D6) and #3 asked for. Where AHSE differs
fundamentally: AHSE keeps two **separate production engines** (RocksDB + LMDB)
and adds a **formal Q\*-Gate cost model** to decide *when* to provision the B+-tree
index — a migration-profitability decision the AHA-tree does not make.
