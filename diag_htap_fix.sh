#!/usr/bin/env bash
# Step 3 targeted re-validation of the re-arming + hysteresis fix.
# Only AHSE is re-run: the fix touches WorkloadMonitor/AdaptiveBrain, which the
# other engines never execute, so their baseline numbers in htap_ci.csv/htap_alt.csv
# are unchanged and reused. Isolated process per run, self-cleaning, telemetry kept.
set -euo pipefail
BIN=./build/htap
LEN=200; VAL=200
rm -f htap_fix.csv htap_fix_alt.csv htap_fix_alt.csv.map
mkdir -p runs diag_htap_fix

# --- (1) size sweep: 2M/5M/10M, 5 reps, baseline cadence 3cyc/500k/100k ---
for KEYS in 2000000 5000000 10000000; do
  for REP in 0 1 2 3 4; do
    TAG="htap_ahse_${KEYS}_${REP}"
    echo ">>> size ahse keys=$KEYS rep=$REP"
    "$BIN" --engine ahse --keys "$KEYS" --cycles 3 \
           --writes-per-cycle 500000 --scans-per-cycle 100000 \
           --scan-len "$LEN" --value-size "$VAL" --runs 1 --out htap_fix.csv >/dev/null 2>&1
    [ "$REP" = "0" ] && cp "runs/${TAG}_telemetry.csv" "diag_htap_fix/ahse_${KEYS}.csv" 2>/dev/null || true
    find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
  done
done

# --- (2) alternation re-check at 2M (AHSE only): fast-flip + rpw25 boundary ---
alt() { # name cycles wpc spc rpw
  local NAME=$1 CYC=$2 WPC=$3 SPC=$4 RPW=$5
  for REP in 0 1 2; do
    TAG="htap_ahse_2000000_${REP}"
    echo ">>> alt[$NAME] ahse rep=$REP"
    "$BIN" --engine ahse --keys 2000000 --cycles "$CYC" \
           --writes-per-cycle "$WPC" --scans-per-cycle "$SPC" \
           --scan-len "$LEN" --value-size "$VAL" --read-phase-write-pct "$RPW" \
           --runs 1 --out htap_fix_alt.csv >/dev/null 2>&1
    echo "${NAME},ahse,${REP}" >> htap_fix_alt.csv.map
    [ "$REP" = "0" ] && cp "runs/${TAG}_telemetry.csv" "diag_htap_fix/alt_${NAME}.csv" 2>/dev/null || true
    find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
  done
}
alt flipfast   6 250000 50000  0
alt skewmilder 3 500000 100000 25
echo "DONE htap_fix"
