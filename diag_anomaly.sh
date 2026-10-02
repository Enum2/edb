#!/usr/bin/env bash
# Root-cause diagnostics for the two anomalies. Preserves AHSE telemetry.
# Telemetry path convention (from main.cpp run_ycsb):
#   tag = <TAG>_<ENGINE>_r<run>   ;   file = runs/<tag>_telemetry.csv
set -euo pipefail
BIN=./build/ahse_bench
mkdir -p diag_tel
rm -f diag_tel/*.csv

copy_tel() { # $1 = TAG base, $2 = dest
  local f="runs/${1}_ahse_r0_telemetry.csv"
  if [ -f "$f" ]; then cp "$f" "$2"; else echo "  (no telemetry file: $f)"; fi
}

# --- Issue 1: Workload A telemetry at each size (does migration EVER fire?) ---
for KEYS in 2000000 3000000 5000000 10000000; do
  echo ">>> [A telemetry] ahse ycsb-a keys=$KEYS"
  "$BIN" --engine ahse --mode ycsb-a --keys "$KEYS" --ops 200000 \
         --value-size 1000 --scan-len 100 --runs 1 --out diag_junk.csv --tag "dA_${KEYS}" >/dev/null 2>&1
  copy_tel "dA_${KEYS}" "diag_tel/A_${KEYS}.csv"
  find runs -maxdepth 1 -name "dA_${KEYS}_*" -exec rm -rf {} + 2>/dev/null || true
done

# --- Issue 2: Workload E at 10M telemetry (when does is_lmdb_ready flip?) ---
echo ">>> [E telemetry] ahse ycsb-e keys=10000000"
"$BIN" --engine ahse --mode ycsb-e --keys 10000000 --ops 200000 \
       --value-size 1000 --scan-len 100 --runs 1 --out diag_junk.csv --tag "dE_10m" >/dev/null 2>&1
copy_tel "dE_10m" "diag_tel/E_10m.csv"
find runs -maxdepth 1 -name "dE_10m_*" -exec rm -rf {} + 2>/dev/null || true

# --- Issue 2 cross-check: Workload E at 5M for comparison ---
echo ">>> [E telemetry] ahse ycsb-e keys=5000000"
"$BIN" --engine ahse --mode ycsb-e --keys 5000000 --ops 200000 \
       --value-size 1000 --scan-len 100 --runs 1 --out diag_junk.csv --tag "dE_5m" >/dev/null 2>&1
copy_tel "dE_5m" "diag_tel/E_5m.csv"
find runs -maxdepth 1 -name "dE_5m_*" -exec rm -rf {} + 2>/dev/null || true

rm -f diag_junk.csv
echo "DONE diag"
