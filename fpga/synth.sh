#!/bin/bash
# B.0 (runs on the build box): run synth_ooc.tcl for one generated config in the Vivado container.
set -e
CFG=${1:-XV6HZedConfig}
F=$HOME/xv6h-fpga
docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v $F:$F -w $F kkrizka/vivado:2020.1 \
  bash -lc "source /opt/Xilinx/Vivado/2020.1/settings64.sh && vivado -mode batch -nojournal -log $F/synth-$CFG/vivado.log -source $F/synth_ooc.tcl -tclargs $F/rtl/$CFG $F/synth-$CFG" > $F/synth-$CFG.out 2>&1
grep -A20 "Slice Logic$" $F/synth-$CFG/util.rpt | head -24
grep -A8 "Memory$" $F/synth-$CFG/util.rpt | head -10
grep -A6 "DSP$" $F/synth-$CFG/util.rpt | head -8
grep -m3 -A3 "WNS(ns)" $F/synth-$CFG/timing.rpt
echo SYNTH_DONE
