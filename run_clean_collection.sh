#!/usr/bin/env bash
#
# run_clean_collection.sh — TURNKEY final re-collection for the paper, to be run
# on a machine WITHOUT endpoint-security/indexing file scanning (dedicated box,
# cloud VM, or a workspace IT-excluded from FortiDLP/Ava/Spotlight).
#
# It (a) logs CPU/mem/top-processes throughout so we have DIRECT evidence of the
# machine state (closes the quiescent-vs-loaded question), (b) re-collects the
# headline HTAP (2M/5M/10M) and the point-read cells B/C/D (all sizes) with the
# --warmup protocol, isolated process per run, self-cleaning.
#
# Usage:   bash run_clean_collection.sh            # full spec
#          SIZES="2000000" REPS=2 bash run_clean_collection.sh   # quick smoke test
#
set -uo pipefail
HTAP=./build/htap
YCSB=./build/ahse_bench
ENGINES="rocksdb lmdb static_hybrid ahse aha"
SIZES="${SIZES:-2000000 3000000 5000000 10000000}"
REPS="${REPS:-5}"
HTAP_SIZES="${HTAP_SIZES:-2000000 5000000 10000000}"
mkdir -p runs; : > runs/.metadata_never_index 2>/dev/null || true

# ---- CPU/machine-state sampler (portable: uses ps + uptime) ----
CPULOG=cpu_samples.log; : > "$CPULOG"
sampler() {
  while :; do
    {
      echo "=== $(date '+%H:%M:%S') ==="
      uptime 2>/dev/null
      # top 5 processes by CPU, and any scanner/agent explicitly
      ps -Ao pcpu,pmem,comm -r 2>/dev/null | head -6
      ps -Ao pcpu,comm -r 2>/dev/null | grep -iE "forti|reveal|ava|spotlight|mds|corespotlight|xprotect" | head -5
    } >> "$CPULOG"
    sleep 5
  done
}
sampler & SAMPLER_PID=$!
trap 'kill $SAMPLER_PID 2>/dev/null' EXIT

clean_run_tag() { find runs -maxdepth 1 -name "$1_*" -exec rm -rf {} + 2>/dev/null || true; }

# ---- Headline HTAP (val=200, alternating W/R), --warmup ----
HOUT=clean_htap.csv; rm -f "$HOUT"
for KEYS in $HTAP_SIZES; do
  for ENG in $ENGINES; do
    for REP in $(seq 0 $((REPS-1))); do
      TAG="htap_${ENG}_${KEYS}_${REP}"
      echo "[$(date '+%H:%M:%S')] HTAP $ENG keys=$KEYS rep=$REP"
      "$HTAP" --engine "$ENG" --keys "$KEYS" --cycles 3 --writes-per-cycle 500000 \
        --scans-per-cycle 100000 --scan-len 200 --value-size 200 --warmup --runs 1 \
        --out "$HOUT" >/dev/null 2>&1 || echo "  nonzero"
      clean_run_tag "$TAG"
    done
  done
done

# ---- Point-read cells B/C/D (val=1000, canonical YCSB), --warmup ----
YOUT=clean_bcde.csv; rm -f "$YOUT"
for KEYS in $SIZES; do
  for W in b c d; do
    for ENG in $ENGINES; do
      for REP in $(seq 0 $((REPS-1))); do
        TAG="yf_${W}_${ENG}_${KEYS}_${REP}"
        echo "[$(date '+%H:%M:%S')] ycsb-$W $ENG keys=$KEYS rep=$REP"
        "$YCSB" --engine "$ENG" --mode "ycsb-$W" --keys "$KEYS" --ops 200000 \
          --value-size 1000 --scan-len 100 --warmup --runs 1 --out "$YOUT" --tag "$TAG" >/dev/null 2>&1 || echo "  nonzero"
        clean_run_tag "$TAG"
      done
    done
  done
done

kill $SAMPLER_PID 2>/dev/null
echo "DONE clean collection. Results: $HOUT, $YOUT. Machine-state log: $CPULOG"
echo "Sanity-check the CPU log shows scanners/agents near-idle throughout:"
grep -iE "forti|reveal|spotlight|mds" "$CPULOG" | awk '{s+=$1;n++} END{if(n)printf "  mean scanner CPU across samples: %.1f%% (want <~10%%)\n",s/n; else print "  (no scanner processes seen — clean)"}'
