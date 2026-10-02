#!/usr/bin/env bash
#
# run_all.sh — process-isolated benchmark runner.
#
# Each repetition is a FRESH process (--runs 1) with freshly-wiped data
# directories, giving true isolation between runs (required by the resubmission
# plan's Experiment 1) and a clean per-run peak-RAM measurement. Every run
# appends one raw row to the output CSV.
#
# Configure via environment variables:
#   ENGINES  space-separated engines   (default: "rocksdb lmdb aha static_hybrid ahse")
#   MODE     ycsb-a|ycsb-b|ycsb-e|custom (default: ycsb-e)
#   KEYS     dataset size               (default: 1000000)
#   OPS      operations / scans         (default: 200000)
#   REPS     repetitions per config     (default: 5)
#   SCANLEN  scan length                (default: 1000)
#   FIXED    "1" for fixed-length scans (default: 1)
#   OUT      output CSV                 (default: results_${MODE}.csv)
#
# Example:
#   MODE=ycsb-e KEYS=5000000 REPS=5 ./run_all.sh
#
set -euo pipefail

BIN="./build/ahse_bench"
[ -x "$BIN" ] || { echo "build first: cmake -S . -B build && cmake --build build -j"; exit 1; }

ENGINES=${ENGINES:-"rocksdb lmdb aha static_hybrid ahse"}
MODE=${MODE:-ycsb-e}
KEYS=${KEYS:-1000000}
OPS=${OPS:-200000}
REPS=${REPS:-5}
SCANLEN=${SCANLEN:-1000}
FIXED=${FIXED:-1}
OUT=${OUT:-results_${MODE}.csv}

FIXED_FLAG=""
[ "$FIXED" = "1" ] && FIXED_FLAG="--fixed-scan"

echo "engines=[$ENGINES] mode=$MODE keys=$KEYS ops=$OPS reps=$REPS scanlen=$SCANLEN out=$OUT"
rm -f "$OUT"

for eng in $ENGINES; do
  for r in $(seq 0 $((REPS - 1))); do
    TAG="iso_${MODE}_${eng}_${r}"
    echo ">>> $eng $MODE keys=$KEYS run=$r"
    "$BIN" --engine "$eng" --mode "$MODE" --keys "$KEYS" --ops "$OPS" \
           --runs 1 --scan-len "$SCANLEN" $FIXED_FLAG \
           --out "$OUT" --tag "$TAG"
    find runs -maxdepth 1 -type d -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
  done
done

echo "done -> $OUT"
