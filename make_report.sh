#!/usr/bin/env bash
# make_report.sh — aggregate all clean_*.csv into one readable report (stdout).
# Called by run_all_wsl.sh; can also be run standalone after the CSVs exist.
set -uo pipefail
cd "$(cd "$(dirname "$0")" && pwd)"

echo "==================================================================="
echo " AHSE — FINAL RESULTS FOR MENTOR"
echo " generated: $(date)"
echo "==================================================================="
echo
echo "MACHINE / ENVIRONMENT"
echo "  host   : $(uname -a 2>/dev/null)"
echo "  cores  : $(nproc 2>/dev/null)"
echo "  memory : $(free -h 2>/dev/null | awk '/Mem/{print $2}')"
echo "  rocksdb: $(grep -m1 ROCKSDB_MAJOR /usr/include/rocksdb/version.h 2>/dev/null | awk '{print $3}')"\
".$(grep -m1 ROCKSDB_MINOR /usr/include/rocksdb/version.h 2>/dev/null | awk '{print $3}')"
echo

echo "MACHINE STATE DURING RUN  (direct evidence — closes quiescent-vs-loaded question)"
if [ -f cpu_samples.log ]; then
  echo -n "  security/indexing agents seen (forti/reveal/ava/spotlight/mds): "
  if grep -iqE "forti|reveal|ava|spotlight|mds|corespotlight" cpu_samples.log; then
    echo "PRESENT — investigate:"; grep -iE "forti|reveal|ava|spotlight|mds" cpu_samples.log | awk '{s+=$1;n++}END{if(n)printf "    mean %.1f%% CPU across %d samples\n",s/n,n}'
  else
    echo "NONE (clean environment)"
  fi
  echo "  peak non-benchmark process CPU across run:"
  grep -vE "===|load average|%CPU|htap|ahse_bench|exp4_concurrency|COMMAND" cpu_samples.log \
    | sort -nr | head -3 | awk '{print "    "$1"%  "$2}'
else
  echo "  (no cpu_samples.log)"
fi
echo

# t_0.975 by n (for 95% CI half-width). n=5 ->2.776, n=3 ->4.303, else ~2.
tval(){ case "$1" in 5) echo 2.776;; 4) echo 3.182;; 3) echo 4.303;; 2) echo 12.706;; *) echo 2.0;; esac; }

echo "-------------------------------------------------------------------"
echo "ITEM 1 — HEADLINE HTAP MIXED WORKLOAD (total_ms, mean, CV%, AHSE margin)"
echo "-------------------------------------------------------------------"
if [ -f clean_htap.csv ]; then
awk -F, 'NR>1{k=$2"_"$1; s[k]+=$9; ss[k]+=$9*$9; n[k]++; ph[$2]=$21}
END{
  ns=split("2000000 5000000 10000000",SZ," "); split("rocksdb lmdb static_hybrid aha ahse",E," ");
  for(si=1;si<=ns;si++){z=SZ[si]; if(!(z"_ahse" in n))continue;
    printf "\n  %d M keys:\n", z/1000000;
    for(ei=1;ei<=5;ei++){e=E[ei]; kk=z"_"e; if(!(kk in n))continue; m=s[kk]/n[kk];
      sd=(n[kk]>1)?sqrt((ss[kk]-n[kk]*m*m)/(n[kk]-1)):0; mean[e]=m;
      printf "    %-14s %8.0f  CV %4.1f%%  (n=%d)\n",e,m,(m>0?100*sd/m:0),n[kk]; }
    if("ahse" in mean){a=mean["ahse"];
      for(ei=1;ei<=4;ei++){e=E[ei]; if(e in mean) printf "      AHSE vs %-14s %+6.1f%%\n",e,100*(mean[e]-a)/mean[e]; }}
    delete mean;
  }
}' clean_htap.csv
else echo "  (clean_htap.csv missing)"; fi
echo

echo "-------------------------------------------------------------------"
echo "ITEM 3a — B/C/D/E (exec_ms mean, CV%, AHSE vs RocksDB)"
echo "-------------------------------------------------------------------"
if [ -f clean_bcde.csv ]; then
awk -F, 'NR>1{k=$2"@"($3/1000000)"M_"$1; s[k]+=$8; ss[k]+=$8*$8; n[k]++}
END{
  split("b c d e",W," "); split("2 3 5 10",S," ");
  for(wi=1;wi<=4;wi++){printf "\n  Workload %s:\n",toupper(W[wi]);
    for(si=1;si<=4;si++){sz=S[si]; w="ycsb-"W[wi];
      ak=w"@"sz"M_ahse"; rk=w"@"sz"M_rocksdb";
      if(!(ak in n))continue; a=s[ak]/n[ak]; asd=(n[ak]>1)?sqrt((ss[ak]-n[ak]*a*a)/(n[ak]-1)):0;
      r=(rk in n)?s[rk]/n[rk]:0;
      printf "    %2dM: AHSE %7.0f (CV %4.1f%%)  RocksDB %7.0f  -> %+6.1f%%\n",
        sz,a,(a>0?100*asd/a:0),r,(r>0?100*(r-a)/r:0); }
  }
}' clean_bcde.csv
else echo "  (clean_bcde.csv missing)"; fi
echo

echo "-------------------------------------------------------------------"
echo "ITEM 3b — CONCURRENCY + CORRECTNESS (Experiment 4)"
echo "-------------------------------------------------------------------"
if [ -f clean_exp4.csv ]; then
awk -F, 'NR>1{w=$1; miss[w]+=$15; mism[w]+=$16; mp99[w]+=$8; sp99[w]+=$12; n[w]++}
END{ printf "  %-8s %-10s %-10s %-14s %-14s\n","writers","missing","mismatch","mig p99(ms)","steady p99(ms)";
  ns=split("1 4 8 16",Wc," "); for(i=1;i<=ns;i++){w=Wc[i]; if(!(w in n))continue;
    printf "  %-8s %-10d %-10d %-14.3f %-14.3f\n",w,miss[w],mism[w],mp99[w]/n[w],sp99[w]/n[w]; }
  print "  (missing=mismatch=0 => zero RocksDB/LMDB divergence under concurrency)"
}' clean_exp4.csv
else echo "  (clean_exp4.csv missing)"; fi
echo

echo "-------------------------------------------------------------------"
echo "ITEM 3c — PARAMETER SENSITIVITY (window_ms, K), HTAP 2M"
echo "-------------------------------------------------------------------"
if [ -f clean_param.csv ] && [ -f clean_param.csv.map ]; then
paste -d, clean_param.csv.map <(awk -F, 'NR>1{print $9","$21}' clean_param.csv) | awk -F, '
{lab=$1;w=$2;k=$3; ms=$5;phys=$6; s[lab]+=ms;ss[lab]+=ms*ms;n[lab]++; W[lab]=w;K[lab]=k; P[lab]=phys}
END{ printf "  %-8s window  K  %-9s %-6s phys\n","config","mean_ms","CV%";
  split("win250 win500 win1000 win2000 K2 K4 K5",O," ");
  for(i=1;i<=7;i++){l=O[i]; if(!(l in n))continue; m=s[l]/n[l]; sd=(n[l]>1)?sqrt((ss[l]-n[l]*m*m)/(n[l]-1)):0;
    printf "  %-8s %-6s  %s  %9.0f %5.1f%%  %s\n",l,W[l],K[l],m,(m>0?100*sd/m:0),P[l]; }
}'
else echo "  (clean_param.csv missing)"; fi
echo
echo "==================================================================="
echo " END OF REPORT"
echo "==================================================================="
