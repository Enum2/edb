#!/usr/bin/env bash
#
# run_all_wsl.sh — ONE-COMMAND full re-collection for the paper, for Windows WSL
# (Ubuntu/Debian) or any clean Linux without endpoint-security file scanning.
#
#   bash run_all_wsl.sh
#
# It (1) installs dependencies, (2) builds, (3) logs CPU throughout, (4) runs the
# whole experiment suite with the warm-up protocol, and (5) writes ONE file,
# RESULTS_FOR_MENTOR.txt, containing every table + machine-state evidence.
# Send that single file to your mentor.
#
# Quick smoke test first (tiny, ~2 min):   SIZES="100000" REPS=1 bash run_all_wsl.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
REPORT="RESULTS_FOR_MENTOR.txt"
REPS="${REPS:-5}"
BCDE_SIZES="${SIZES:-2000000 3000000 5000000 10000000}"
HTAP_SIZES="${HTAP_SIZES:-2000000 5000000 10000000}"
ENGINES="rocksdb lmdb static_hybrid ahse aha"
log(){ echo "[$(date +%H:%M:%S)] $*"; }

# ---------- 1. dependencies (Ubuntu/Debian) ----------
if ! dpkg -s librocksdb-dev >/dev/null 2>&1; then
  log "Installing dependencies via apt (needs sudo once)..."
  sudo apt-get update -y
  sudo apt-get install -y build-essential cmake liblmdb-dev librocksdb-dev \
       libabsl-dev libsnappy-dev libgflags-dev libzstd-dev liblz4-dev libbz2-dev || {
    echo "apt install failed — install build-essential cmake liblmdb-dev librocksdb-dev libabsl-dev manually"; exit 1; }
fi

# ---------- 2. build ----------
log "Configuring + building..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/tmp/ahse_cmake.log 2>&1 || { echo "CMAKE FAILED:"; tail -25 /tmp/ahse_cmake.log; exit 1; }
cmake --build build -j >/tmp/ahse_build.log 2>&1 || { echo "BUILD FAILED:"; tail -30 /tmp/ahse_build.log; exit 1; }
log "Build OK."

# ---------- 3. CPU/machine-state sampler ----------
: > cpu_samples.log
( while :; do { echo "=== $(date +%H:%M:%S) ==="; uptime;
      ps -eo pcpu,comm --sort=-pcpu 2>/dev/null | head -6; } >> cpu_samples.log; sleep 5; done ) &
SAMPLER=$!; trap 'kill $SAMPLER 2>/dev/null' EXIT
mkdir -p runs
cl(){ find runs -mindepth 1 -maxdepth 1 -name "$1*" -exec rm -rf {} + 2>/dev/null; }

# ---------- 4a. Headline HTAP (val=200), warm-up ----------
rm -f clean_htap.csv
for K in $HTAP_SIZES; do for E in $ENGINES; do for R in $(seq 0 $((REPS-1))); do
  log "HTAP $E keys=$K rep=$R"
  ./build/htap --engine "$E" --keys "$K" --cycles 3 --writes-per-cycle 500000 \
    --scans-per-cycle 100000 --scan-len 200 --value-size 200 --warmup --runs 1 \
    --out clean_htap.csv >/dev/null 2>&1 || log "  (nonzero)"
  cl "htap_${E}_${K}_${R}"
done; done; done

# ---------- 4b. Point-read + scan B/C/D/E (val=1000), warm-up ----------
rm -f clean_bcde.csv
for K in $BCDE_SIZES; do for W in b c d e; do for E in $ENGINES; do for R in $(seq 0 $((REPS-1))); do
  log "ycsb-$W $E keys=$K rep=$R"
  ./build/ahse_bench --engine "$E" --mode "ycsb-$W" --keys "$K" --ops 200000 \
    --value-size 1000 --scan-len 100 --warmup --runs 1 --out clean_bcde.csv \
    --tag "yf_${W}_${E}_${K}_${R}" >/dev/null 2>&1 || log "  (nonzero)"
  cl "yf_${W}_${E}_${K}_${R}"
done; done; done; done

# ---------- 4c. Concurrency + correctness (Exp-4) ----------
rm -f clean_exp4.csv
log "Exp-4 concurrency (1/4/8/16 writers)"
./build/exp4_concurrency --keys 2000000 --value-size 1000 --reps 5 --out clean_exp4.csv >/dev/null 2>&1 || log "  (nonzero)"

# ---------- 4d. Parameter sensitivity (window_ms, K) ----------
rm -f clean_param.csv clean_param.csv.map
for cfg in "win250 250 3" "win500 500 3" "win1000 1000 3" "win2000 2000 3" "K2 500 2" "K4 500 4" "K5 500 5"; do
  set -- $cfg; L=$1; W=$2; KK=$3
  for R in 0 1 2; do
    log "param $L (window=$W K=$KK) rep=$R"
    AHSE_WINDOW_MS=$W AHSE_K=$KK ./build/htap --engine ahse --keys 2000000 --cycles 3 \
      --writes-per-cycle 500000 --scans-per-cycle 100000 --scan-len 200 --value-size 200 \
      --warmup --runs 1 --out clean_param.csv >/dev/null 2>&1 || log "  (nonzero)"
    echo "$L,$W,$KK,$R" >> clean_param.csv.map
    cl "htap_ahse_2000000_${R}"
  done
done

kill $SAMPLER 2>/dev/null
log "All runs done. Building report..."
bash "$ROOT/make_report.sh" > "$REPORT"
echo
echo "=================================================================="
echo " DONE.  Send this ONE file to your mentor:  $ROOT/$REPORT"
echo "=================================================================="
