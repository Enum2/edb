#!/usr/bin/env bash
# Finish the sweep: AHSE 10M ycsb-e x5 with the teardown-race fix.
set -uo pipefail   # NOT -e, so one failure doesn't abort the batch
BIN=./build/ahse_bench
OUT=ycsb_bcde.csv
for REP in 0 1 2 3 4; do
  TAG="yf_e_ahse_10000000_${REP}fx"
  echo ">>> ycsb-e ahse keys=10000000 rep=$REP"
  "$BIN" --engine ahse --mode ycsb-e --keys 10000000 --ops 200000 \
         --value-size 1000 --scan-len 100 --runs 1 --out "$OUT" --tag "$TAG" >/dev/null 2>&1 \
    || echo "   run rep=$REP exited nonzero"
  find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
done
echo "DONE ahse10me"
