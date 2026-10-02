#!/usr/bin/env bash
# Item 3c: parameter sensitivity on window_ms and the K-of-5 consensus threshold.
# AHSE only (baselines don't depend on these), HTAP 2M, val=200 (light churn),
# warm-up, 3 reps/config, self-cleaning, short settle between runs.
set -uo pipefail
BIN=./build/htap
OUT=param3c.csv; rm -f "$OUT" "$OUT.map"
: > runs/.metadata_never_index 2>/dev/null || true
run_cfg() { # label WINDOW K
  local LABEL=$1 W=$2 K=$3
  for REP in 0 1 2; do
    TAG="htap_ahse_2000000_${REP}"
    echo ">>> [$LABEL] window=$W K=$K rep=$REP"
    AHSE_WINDOW_MS=$W AHSE_K=$K "$BIN" --engine ahse --keys 2000000 --cycles 3 \
      --writes-per-cycle 500000 --scans-per-cycle 100000 --scan-len 200 \
      --value-size 200 --warmup --runs 1 --out "$OUT" >/dev/null 2>&1 \
      || echo "  $LABEL rep$REP nonzero"
    echo "$LABEL,$W,$K,$REP" >> "$OUT.map"
    find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
    sleep 2   # brief settle so the file-scanners don't back up
  done
}
# window_ms sweep (K=3 fixed)
run_cfg win250   250  3
run_cfg win500   500  3      # default
run_cfg win1000  1000 3
run_cfg win2000  2000 3
# K sweep (window=500 fixed)
run_cfg K2       500  2
run_cfg K4       500  4
run_cfg K5       500  5
echo "DONE 3c"
