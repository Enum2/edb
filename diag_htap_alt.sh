#!/usr/bin/env bash
# HTAP alternation-pattern sweep (mentor item 4), all at 2M keys.
# Two independent axes, isolated process per run, self-cleaning:
#
#  A) FLIP FREQUENCY  - total work held ~constant (1.5M writes, 300k scans);
#     only the number of phase flips changes:
#       fast    : 6 cycles x (250k W, 50k S)
#       baseline: 3 cycles x (500k W, 100k S)   [= item-1 config]
#       slow    : 2 cycles x (750k W, 150k S)
#
#  B) PHASE SKEW      - baseline cadence (3 cycles), soften the read phase by
#     injecting writes into the analytics burst:
#       sharp  : rpw=0   (~100% read phase)   [= item-1 config]
#       mild   : rpw=15  (~87% read phase)
#       milder : rpw=25  (~80% read phase, at the r>0.80 trigger boundary)
#
# 4 engines (rocksdb/lmdb/static_hybrid/ahse), 3 reps each (exploratory CI).
set -euo pipefail
BIN=./build/htap
OUT=htap_alt.csv
KEYS=2000000; LEN=200; VAL=200; REPS=3
ENGINES="rocksdb lmdb static_hybrid ahse"

rm -f "$OUT"
mkdir -p runs

run_cfg() { # name cycles wpc spc rpw
  local NAME=$1 CYC=$2 WPC=$3 SPC=$4 RPW=$5
  for ENG in $ENGINES; do
    for REP in $(seq 0 $((REPS-1))); do
      TAG="htap_${ENG}_${KEYS}_${REP}"
      echo ">>> [$NAME] $ENG rep=$REP (cyc=$CYC wpc=$WPC spc=$SPC rpw=$RPW)"
      "$BIN" --engine "$ENG" --keys "$KEYS" --cycles "$CYC" \
             --writes-per-cycle "$WPC" --scans-per-cycle "$SPC" \
             --scan-len "$LEN" --value-size "$VAL" --read-phase-write-pct "$RPW" \
             --runs 1 --out "$OUT" >/dev/null 2>&1
      # tag the config name into a side file (CSV has no config-name column)
      echo "${NAME},${ENG},${REP}" >> "${OUT}.map"
      find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
    done
  done
}

rm -f "${OUT}.map"
# Axis A: flip frequency
run_cfg "flipfast"  6 250000 50000  0
run_cfg "flipslow"  2 750000 150000 0
# Axis B: phase skew (baseline cadence)
run_cfg "skewmild"   3 500000 100000 15
run_cfg "skewmilder" 3 500000 100000 25
echo "DONE htap_alt"
