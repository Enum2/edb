#!/usr/bin/env bash
#
# run_htap.sh — HTAP end-to-end comparison, all engines, 5 repetitions.
#
# Alternating ingestion/analytics. Each engine is its own set of process
# invocations (isolation); htap.replays an identical operation sequence per
# repetition across engines. Reports one raw row per run to htap_results.csv.
#
set -euo pipefail
BIN=./build/htap

KEYS=${KEYS:-2000000}
CYCLES=${CYCLES:-3}
WPC=${WPC:-500000}
SPC=${SPC:-100000}
LEN=${LEN:-200}
VAL=${VAL:-200}
REPS=${REPS:-5}
OUT=${OUT:-htap_results.csv}
ENGINES=${ENGINES:-"rocksdb lmdb static_hybrid ahse"}

rm -f "$OUT"
for E in $ENGINES; do
  echo ">>> HTAP $E (reps=$REPS)"
  "$BIN" --engine "$E" --keys "$KEYS" --cycles "$CYCLES" \
         --writes-per-cycle "$WPC" --scans-per-cycle "$SPC" \
         --scan-len "$LEN" --value-size "$VAL" --runs "$REPS" --out "$OUT"
done
echo "DONE -> $OUT"
