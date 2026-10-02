#!/usr/bin/env bash
# Item 4: isolate the monitoring tax on Workload C @ 10M (pure point reads).
# Same fresh batch: rocksdb (ref), ahse (monitor ON), ahse (monitor OFF).
set -uo pipefail
BIN=./build/ahse_bench
OUT=montax.csv; rm -f "$OUT"
run() { local LABEL=$1 ENG=$2 NOMON=$3 REP=$4; local TAG="mt_${LABEL}_${REP}"
  echo ">>> $LABEL rep=$REP"
  if [ "$NOMON" = "1" ]; then export AHSE_NO_MONITOR=1; else unset AHSE_NO_MONITOR; fi
  "$BIN" --engine "$ENG" --mode ycsb-c --keys 10000000 --ops 200000 \
         --value-size 1000 --scan-len 100 --runs 1 --out "$OUT" --tag "$TAG" >/dev/null 2>&1 \
    || echo "  rep$REP nonzero"
  unset AHSE_NO_MONITOR
  find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
}
for REP in 0 1 2 3 4; do run rocksdb        rocksdb 0 "$REP"; done
for REP in 0 1 2 3 4; do run ahse_monon     ahse    0 "$REP"; done
for REP in 0 1 2 3 4; do run ahse_monoff    ahse    1 "$REP"; done
echo "DONE montax"
