#!/usr/bin/env bash
# (0) 5 extra HTAP-2M reps (final fix set) to characterize the phys 2/3
#     bimodality with 10 total reps.
# (1) YCSB B/C/D/E x 2M/3M/5M/10M x 5 engines x 5 reps, final combined fix set,
#     isolated process per run, self-cleaning, 1KB records (canonical protocol).
set -euo pipefail
HTAP=./build/htap
BIN=./build/ahse_bench
LEN=200; VAL_H=200

# --- (0) extra HTAP 2M reps ---
rm -f htap_gate_extra.csv
for REP in 5 6 7 8 9; do
  TAG="htap_ahse_2000000_${REP}"
  echo ">>> HTAP2M extra rep=$REP"
  "$HTAP" --engine ahse --keys 2000000 --cycles 3 --writes-per-cycle 500000 \
          --scans-per-cycle 100000 --scan-len "$LEN" --value-size "$VAL_H" \
          --runs 1 --out htap_gate_extra.csv >/dev/null 2>&1
  find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
done
echo "=== HTAP extra done ==="

# --- (1) YCSB B/C/D/E sweep ---
OUT=ycsb_bcde.csv
rm -f "$OUT"
for KEYS in 2000000 3000000 5000000 10000000; do
  for W in b c d e; do
    for E in rocksdb lmdb aha static_hybrid ahse; do
      for REP in 0 1 2 3 4; do
        TAG="yf_${W}_${E}_${KEYS}_${REP}"
        echo ">>> ycsb-$W $E keys=$KEYS rep=$REP"
        "$BIN" --engine "$E" --mode "ycsb-$W" --keys "$KEYS" --ops 200000 \
               --value-size 1000 --scan-len 100 --runs 1 --out "$OUT" --tag "$TAG" >/dev/null 2>&1
        find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
      done
    done
  done
  echo "=== SIZE $KEYS COMPLETE ==="
done
echo "DONE bcde"
