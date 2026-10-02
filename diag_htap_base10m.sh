#!/usr/bin/env bash
# Fresh, same-batch baselines at 10M so the pivotal AHSE-vs-baseline verdict is
# apples-to-apples with the freshly-run gate AHSE numbers (removes any
# warm-vs-cold cache objection). Isolated process/run, self-cleaning.
set -euo pipefail
BIN=./build/htap
LEN=200; VAL=200
rm -f htap_base10m.csv
mkdir -p runs
for ENG in rocksdb lmdb static_hybrid aha; do
  for REP in 0 1 2 3 4; do
    TAG="htap_${ENG}_10000000_${REP}"
    echo ">>> $ENG 10M rep=$REP"
    "$BIN" --engine "$ENG" --keys 10000000 --cycles 3 \
           --writes-per-cycle 500000 --scans-per-cycle 100000 \
           --scan-len "$LEN" --value-size "$VAL" --runs 1 --out htap_base10m.csv >/dev/null 2>&1
    find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
  done
done
echo "DONE base10m"
