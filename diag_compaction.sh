#!/usr/bin/env bash
# Q2: correlate C@10M per-rep latency with RocksDB compaction state.
# Fresh dir per rep (RocksDBEngine wipes on construct); AHSE_LOG_COMPACTION dumps
# compaction counters pre/post the timed window. rocksdb x15, ahse x5.
set -uo pipefail
BIN=./build/ahse_bench
: > compaction.log
export AHSE_LOG_COMPACTION=1
run() { local ENG=$1 REP=$2; local TAG="cmp_${ENG}_${REP}"
  "$BIN" --engine "$ENG" --mode ycsb-c --keys 10000000 --ops 200000 \
    --value-size 1000 --scan-len 100 --runs 1 --out /tmp/cmp.csv --tag "$TAG" 2>>compaction.log >/dev/null
  find runs -maxdepth 1 -name "${TAG}_*" -exec rm -rf {} + 2>/dev/null || true
}
for REP in $(seq 1 15); do run rocksdb "$REP"; done
for REP in $(seq 1 5);  do run ahse "$REP"; done
echo "DONE compaction" >> compaction.log
rm -f /tmp/cmp.csv
