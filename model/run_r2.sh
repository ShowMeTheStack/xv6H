#!/bin/sh
# the model with MACH call 7 (rate_now). Resumable: a property whose output already
# has its final statistics is not run again, so a reboot costs only the search in progress.
# Runs in ~/xv6h-spin/r2 on the build box.
#   2x2: SAFETY and the three liveness properties, exhaustive (as Table 7's first rows)
#   2x2: the two variants that must fail SAFETY (-DRATE_AT_ASK: defect 49; -DNO_CALL7)
#   3x2: SAFETY exhaustive under COLLAPSE (60 GB); then the three liveness properties by bitstate
export PATH=$HOME/spin/Spin-master/Src:$PATH
M=$(cd "$(dirname "$0")" && pwd)/revision_n.pml
finished() { grep -q "elapsed time" "$1" 2>/dev/null && grep -q "errors:" "$1" 2>/dev/null; }
mk() { # DIR DEFINES...
  d=$1; shift; mkdir -p $d; cp $M $d/m.pml; (cd $d && spin -a "$@" m.pml >/dev/null); }

mk n2f2 -DNH=2 -DNF=2
cd n2f2
finished safety.out || { gcc -O2 -DSAFETY -o pan_s pan.c && ./pan_s -m400000 > safety.out 2>&1; }
[ -x pan_l ] || gcc -O2 -DNFAIR=7 -o pan_l pan.c
for p in adopted no_owed caught_up; do finished live_$p.out || ./pan_l -m400000 -a -f -N $p > live_$p.out 2>&1; done
cd ..
for v in RATE_AT_ASK NO_CALL7; do
  mk var_$v -DNH=2 -DNF=2 -D$v
  (cd var_$v && { finished safety.out || { gcc -O2 -DSAFETY -o pan_s pan.c && ./pan_s -m400000 > safety.out 2>&1; }; })
done
mk n3f2 -DNH=3 -DNF=2
cd n3f2
finished safety.out || { gcc -O2 -DSAFETY -DCOLLAPSE -DMEMLIM=60000 -o pan_s pan.c && ./pan_s -m400000 > safety.out 2>&1; }
[ -x pan_b ] || gcc -O2 -DBITSTATE -DNFAIR=9 -o pan_b pan.c
for p in adopted no_owed caught_up; do finished bit_$p.out || ./pan_b -m400000 -w36 -a -f -N $p > bit_$p.out 2>&1 & done
wait
cd ..
touch done
