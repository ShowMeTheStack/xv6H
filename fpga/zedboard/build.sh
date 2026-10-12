#!/bin/bash
# The ZedBoard bitstream for one generated config, in the Vivado container.
#   bash build.sh [XV6HZedSoCConfig]
set -e
CFG=${1:-XV6HZedSoCConfig}
F=$HOME/xv6h-fpga; Z=$F/zedboard; OUT=$F/zed-$CFG
python3 $Z/mkwrap.py $F/rtl/$CFG/ExampleRocketSystem.sv > $Z/rocket_wrap.v
rm -rf $OUT; mkdir -p $OUT
docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v $F:$F -w $OUT kkrizka/vivado:2020.1 \
  bash -lc "source /opt/Xilinx/Vivado/2020.1/settings64.sh && vivado -mode batch -nojournal -log $OUT/vivado.log -source $Z/build.tcl -tclargs $F/rtl/$CFG $OUT" > $OUT.out 2>&1 || true
grep -E "^ADDR|ERROR|CRITICAL WARNING" $OUT/vivado.log | head -40
grep -A20 "Slice Logic$" $OUT/util.rpt 2>/dev/null | head -24
grep -m3 -A3 "WNS(ns)" $OUT/timing.rpt 2>/dev/null
ls -la $OUT/xv6hzed.bit 2>/dev/null
echo ZED_BUILD_DONE
