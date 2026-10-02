#!/usr/bin/env bash
# Issue 1 rigor: 5 repetitions of Workload A at 3M and 5M for rocksdb vs ahse,
# each repetition a SEPARATE process (full isolation), self-cleaning per run so
# the DB dirs never accumulate. Produces one raw row per repetition.
set -euo pipefail
BIN=./build/ahse_bench
OUT=issue1_ci.csv
rm -f "$OUT"
mkdir -p runs

for KEYS in 3000000 5000000; do
  for ENG in rocksdb ahse; do
    for REP in 0 1 2 3 4; do
      TAG="ci_${KEYS}_${ENG}_${REP}"
      echo ">>> $ENG keys=$KEYS rep=$REP"
      "$BIN" --engine "$ENG" --mode ycsb-a --keys "$KEYS" --ops 200000 \
             --value-size 1000 --scan-len 100 --runs 1 \
             --out "$OUT" --tag "$TAG" >/dev/null 2>&1
      # self-clean this run's data dirs immediately (avoid disk bloat)
      find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
    done
  done
done
echo "DONE issue1_ci"
