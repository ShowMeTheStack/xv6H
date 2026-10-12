#!/bin/sh
# Resumable model-checking runs: NH=3 NF=3 bitstate liveness where it is missing, then NH=4 NF=2
# (exhaustive safety under COLLAPSE with 40 GB, bitstate safety if that cannot finish, bitstate
# liveness). A property whose output already ends with an elapsed time is not run again, so a
# reboot costs only the search in progress. Runs in ~/xv6h-spin as the user service xv6h-spin4.
export PATH=$HOME/spin/Spin-master/Src:$PATH
finished() { grep -q "elapsed time" "$1" 2>/dev/null && grep -q "errors:" "$1" 2>/dev/null; }
for c in "3 3" "4 2"; do set -- $c
  d=n$1f$2; mkdir -p $d; cp revision_n.pml $d/; cd $d
  spin -a -DNH=$1 -DNF=$2 revision_n.pml >/dev/null
  if [ "$d" = n4f2 ] && ! finished safety.out; then
    gcc -O2 -DSAFETY -DCOLLAPSE -DMEMLIM=40000 -o pan_s pan.c && ./pan_s -m400000 -N gen_owner > safety.out 2>&1
  fi
  if grep -q "Search not completed" safety.out 2>/dev/null && ! finished bit_safety.out; then
    gcc -O2 -DSAFETY -DBITSTATE -o pan_bs pan.c && ./pan_bs -m400000 -w36 -N gen_owner > bit_safety.out 2>&1
  fi
  [ -x pan_b ] || gcc -O2 -DBITSTATE -DNFAIR=$((2*$1+3)) -o pan_b pan.c
  for p in adopted no_owed caught_up; do
    finished bit_$p.out || ./pan_b -m400000 -w36 -a -f -N $p > bit_$p.out 2>&1
  done
  cd ..
done
touch done4
