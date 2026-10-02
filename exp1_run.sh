#!/usr/bin/env bash
#
# exp1_run.sh — Experiment 1 full data collection.
#
# 1) Process-ISOLATED Workload A: rocksdb vs ahse, each engine in its own
#    process invocation (removes the in-process ordering/warmup bias present in
#    the original ycsb_benchmark.cpp). Sizes 1M / 5M / 10M.
# 2) LEAKY demonstration (H1 mechanism) at 1M, in-process by design.
#
set -euo pipefail
BIN=./build/ahse_bench
EXP1=./build/exp1_workload_a

rm -f exp1_isoA.csv exp1_leaky.csv

for KEYS in 1000000 5000000 10000000; do
  if [ "$KEYS" -le 1000000 ]; then REPS=5; OPS=200000;
  elif [ "$KEYS" -le 5000000 ]; then REPS=5; OPS=500000;
  else REPS=3; OPS=500000; fi
  for ENG in rocksdb ahse; do
    for r in $(seq 0 $((REPS-1))); do
      echo ">>> iso Workload A | $ENG keys=$KEYS ops=$OPS run=$r"
      "$BIN" --engine "$ENG" --mode ycsb-a --keys "$KEYS" --ops "$OPS" \
             --runs 1 --out exp1_isoA.csv --tag "e1_${ENG}_${KEYS}_${r}"
    done
  done
done

echo ">>> LEAKY H1 demonstration @1M"
"$EXP1" --keys 1000000 --ops 200000 --reps 5 --out exp1_leaky.csv

echo "DONE exp1"
