#!/usr/bin/env bash
# Item 2: stress-test the teardown-race fix. Run AHSE 10M ycsb-e 20 times,
# capture each exit code, count segfaults (exit 139) / any nonzero. Self-cleaning.
set -uo pipefail
BIN=./build/ahse_bench
PASS=0; FAIL=0
: > stress20.log
for i in $(seq 1 20); do
  TAG="st_${i}"
  ./build/ahse_bench --engine ahse --mode ycsb-e --keys 10000000 --ops 200000 \
    --value-size 1000 --scan-len 100 --runs 1 --out /tmp/stress_out.csv --tag "$TAG" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
  echo "run $i: exit=$rc (pass=$PASS fail=$FAIL)" | tee -a stress20.log
  find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
done
echo "STRESS DONE: pass=$PASS fail=$FAIL" | tee -a stress20.log
rm -f /tmp/stress_out.csv
