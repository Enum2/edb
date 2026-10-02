#!/usr/bin/env bash
#
# run_figures.sh — regenerate the custom 7-size throughput/latency figures.
#
# Write + scan performance across dataset sizes for all engines, isolated runs.
# NOTE: 25M/40M with LMDB / static_hybrid are very slow (single-writer LMDB load
# of tens of GB). Override SIZES to stage the long runs separately, e.g.:
#   SIZES="1000000 3000000 5000000 8000000 10000000" ./run_figures.sh
#   SIZES="25000000 40000000" ENGINES="rocksdb aha ahse" ./run_figures.sh
#
set -euo pipefail
BIN=./build/ahse_bench
OUT=${OUT:-figures_results.csv}
REPS=${REPS:-3}
OPS=${OPS:-20000}
VAL=${VAL:-1000}
LEN=${LEN:-100}
SIZES=${SIZES:-"1000000 3000000 5000000 8000000 10000000 25000000 40000000"}
ENGINES=${ENGINES:-"rocksdb lmdb aha static_hybrid ahse"}

rm -f "$OUT"
for KEYS in $SIZES; do
  for E in $ENGINES; do
    for r in $(seq 0 $((REPS-1))); do
      TAG="fig_${E}_${KEYS}_${r}"
      echo ">>> custom $E keys=$KEYS run=$r"
      "$BIN" --engine "$E" --mode custom --keys "$KEYS" --ops "$OPS" \
             --value-size "$VAL" --scan-len "$LEN" --fixed-scan --shuffle-load \
             --runs 1 --out "$OUT" --tag "$TAG"
      find runs -maxdepth 1 -type d -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
    done
  done
done
echo "DONE figures -> $OUT"
