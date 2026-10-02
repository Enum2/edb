#!/usr/bin/env bash
#
# run_sweep.sh — honest scan-latency characterization: RocksDB vs LMDB across
# the (record size) x (scan length) grid. This is the figure that shows WHY the
# scan-latency advantage is workload-dependent (iteration-bound vs data-bound)
# rather than a single inflated speedup. Writes materialize; random load order.
#
set -euo pipefail
BIN=./build/ahse_bench

KEYS=${KEYS:-2000000}
OPS=${OPS:-20000}
REPS=${REPS:-3}
OUT=${OUT:-sweep_results.csv}
VALUES=${VALUES:-"16 100 1000"}
LENS=${LENS:-"10 100 1000"}

rm -f "$OUT"
for V in $VALUES; do
  for L in $LENS; do
    for E in rocksdb lmdb; do
      for r in $(seq 0 $((REPS-1))); do
        echo ">>> sweep $E val=$V len=$L run=$r"
        "$BIN" --engine "$E" --mode custom --keys "$KEYS" --ops "$OPS" \
               --scan-len "$L" --fixed-scan --value-size "$V" --shuffle-load \
               --runs 1 --out "$OUT" --tag "sw_${E}_${V}_${L}_${r}"
      done
    done
  done
done
echo "DONE -> $OUT"
