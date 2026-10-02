#!/usr/bin/env bash
#
# run_table1.sh — regenerate YCSB Table 1 under the isolated harness.
#
# YCSB A/B/E at 5M and 10M for all five engines, one process per run, 1 KB
# records (YCSB standard). Workload E uses standard YCSB-E scan lengths (1..100,
# not the paper's fixed-1000), which is the more defensible setting.
#
set -euo pipefail
BIN=./build/ahse_bench
OUT=${OUT:-table1_results.csv}
REPS=${REPS:-3}
OPS=${OPS:-200000}
VAL=${VAL:-1000}
SIZES=${SIZES:-"5000000 10000000"}
ENGINES=${ENGINES:-"rocksdb lmdb aha static_hybrid ahse"}

rm -f "$OUT"
for KEYS in $SIZES; do
  for WL in ycsb-a ycsb-b ycsb-e; do
    for E in $ENGINES; do
      for r in $(seq 0 $((REPS-1))); do
        TAG="t1_${WL}_${E}_${KEYS}_${r}"
        echo ">>> $WL $E keys=$KEYS run=$r"
        "$BIN" --engine "$E" --mode "$WL" --keys "$KEYS" --ops "$OPS" \
               --value-size "$VAL" --scan-len 100 --runs 1 \
               --out "$OUT" --tag "$TAG"
        # Reclaim this run's data dirs so accumulated GBs don't pollute the
        # page cache and skew later runs (keeps only the telemetry CSV).
        find runs -maxdepth 1 -type d -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
      done
    done
  done
done
echo "DONE table1 -> $OUT"
