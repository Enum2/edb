#!/usr/bin/env bash
# Resume the killed sweep: finish 10M ycsb-e only.
# Already done: rocksdb x5, lmdb x1. Remaining: lmdb reps 1-4, aha/static_hybrid/ahse x5.
set -euo pipefail
BIN=./build/ahse_bench
OUT=ycsb_bcde.csv
K=10000000
run() { local E=$1 REP=$2; local TAG="yf_e_${E}_${K}_${REP}r";
  echo ">>> ycsb-e $E keys=$K rep=$REP"
  "$BIN" --engine "$E" --mode ycsb-e --keys "$K" --ops 200000 \
         --value-size 1000 --scan-len 100 --runs 1 --out "$OUT" --tag "$TAG" >/dev/null 2>&1
  find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
}
for REP in 1 2 3 4; do run lmdb "$REP"; done
for E in aha static_hybrid ahse; do for REP in 0 1 2 3 4; do run "$E" "$REP"; done; done
echo "DONE resume"
