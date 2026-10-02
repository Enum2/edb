#!/usr/bin/env bash
# Item 1: headline HTAP mixed workload, re-run with WARM-UP protocol.
# 2M/5M/10M, all 5 engines, isolated process per run, 5 reps, self-cleaning.
set -uo pipefail
BIN=./build/htap
OUT=htap_warm.csv; rm -f "$OUT"
CYC=3; WPC=500000; SPC=100000; LEN=200; VAL=200; REPS=5
: > headline.log
for KEYS in 2000000 5000000 10000000; do
  for ENG in rocksdb lmdb static_hybrid ahse aha; do
    for REP in $(seq 0 $((REPS-1))); do
      TAG="htap_${ENG}_${KEYS}_${REP}"
      echo ">>> $ENG keys=$KEYS rep=$REP" >> headline.log
      "$BIN" --engine "$ENG" --keys "$KEYS" --cycles "$CYC" \
             --writes-per-cycle "$WPC" --scans-per-cycle "$SPC" \
             --scan-len "$LEN" --value-size "$VAL" --warmup --runs 1 \
             --out "$OUT" >/dev/null 2>&1 || echo "  $ENG $KEYS rep$REP nonzero" >> headline.log
      find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
    done
  done
  echo "=== SIZE $KEYS DONE ===" >> headline.log
done
echo "DONE headline" >> headline.log
