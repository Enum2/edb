#!/usr/bin/env bash
#
# run_ycsb_full.sh — full YCSB suite (A–F) across dataset sizes, all engines.
#
# Workloads: A (50/50), B (95/5 read), C (100% read), D (95/5 read+insert),
#            E (95% scan), F (50% read-modify-write).
# One process per run, data dirs cleaned between runs, 1 KB records.
# Ordered size-outer so each dataset size completes as a full block.
#
set -euo pipefail
BIN=./build/ahse_bench
OUT=${OUT:-ycsb_full_results.csv}
REPS=${REPS:-3}
OPS=${OPS:-200000}
VAL=${VAL:-1000}
SIZES=${SIZES:-"2000000 3000000 4000000 5000000"}
WORKLOADS=${WORKLOADS:-"a b c d e f"}
ENGINES=${ENGINES:-"rocksdb lmdb aha static_hybrid ahse"}

rm -f "$OUT"
for KEYS in $SIZES; do
  for E in $ENGINES; do
    for W in $WORKLOADS; do
      for r in $(seq 0 $((REPS-1))); do
        TAG="yf_${W}_${E}_${KEYS}_${r}"
        echo ">>> ycsb-$W $E keys=$KEYS run=$r"
        "$BIN" --engine "$E" --mode "ycsb-$W" --keys "$KEYS" --ops "$OPS" \
               --value-size "$VAL" --scan-len 100 --runs 1 \
               --out "$OUT" --tag "$TAG"
        find runs -maxdepth 1 -type d -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
      done
    done
  done
  echo "=== SIZE $KEYS COMPLETE ==="
done
echo "DONE ycsb_full -> $OUT"
