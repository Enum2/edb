#!/usr/bin/env bash
# HTAP CI rigor (mentor items 1 + 3): isolated process PER run, 5 reps, all 5
# engines, at 2M / 5M / 10M. Self-cleans each run's data dirs so disk never
# accumulates. Preserves the AHSE per-tick telemetry for item 2 (one rep/size).
set -euo pipefail
BIN=./build/htap
OUT=htap_ci.csv
CYCLES=3; WPC=500000; SPC=100000; LEN=200; VAL=200; REPS=5
ENGINES="rocksdb lmdb static_hybrid ahse aha"
SIZES="${SIZES:-2000000 5000000 10000000}"

rm -f "$OUT"
mkdir -p runs diag_htap

for KEYS in $SIZES; do
  for ENG in $ENGINES; do
    for REP in $(seq 0 $((REPS-1))); do
      TAG="htap_${ENG}_${KEYS}_${REP}"   # matches htap.cpp's internal tag
      echo ">>> $ENG keys=$KEYS rep=$REP"
      "$BIN" --engine "$ENG" --keys "$KEYS" --cycles "$CYCLES" \
             --writes-per-cycle "$WPC" --scans-per-cycle "$SPC" \
             --scan-len "$LEN" --value-size "$VAL" --runs 1 \
             --out "$OUT" >/dev/null 2>&1
      # preserve AHSE telemetry for the first rep of each size (item 2)
      if [ "$ENG" = "ahse" ] && [ "$REP" = "0" ]; then
        cp "runs/${TAG}_telemetry.csv" "diag_htap/ahse_${KEYS}.csv" 2>/dev/null || true
      fi
      find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
    done
  done
  echo "=== size $KEYS complete ==="
done
echo "DONE htap_ci"
