#!/bin/bash
# Elaborate a config (arg 1) with ExampleRocketSystem as top, emit Vivado-safe SystemVerilog
# with the memories as external macros, and generate block-RAM implementations for them.
set -e
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 PATH=$HOME/xv6h-fpga/bin:$JAVA_HOME/bin:$PATH
CFG=${1:-XV6HZedSoCConfig}
cd ~/xv6h-fpga/rocket-chip
time mill -i "emulator[freechips.rocketchip.system.ExampleRocketSystem,freechips.rocketchip.system.$CFG].generator.elaborate"
E=out/emulator/freechips.rocketchip.system.ExampleRocketSystem/freechips.rocketchip.system.$CFG/generator/elaborate.dest
O=$HOME/xv6h-fpga/rtl/$CFG
rm -rf $O; mkdir -p $O
firtool $E/ExampleRocketSystem.fir --annotation-file=$E/ExampleRocketSystem.anno.json --disable-annotation-unknown \
  -O=release --split-verilog --repl-seq-mem --repl-seq-mem-file=mems.conf \
  --lowering-options=disallowLocalVariables,disallowPackedArrays,locationInfoStyle=none,noAlwaysComb -o $O
python3 $HOME/xv6h-fpga/memgen.py $O/mems.conf > $O/mems.v
cp $E/*.dts $O/rocket.dts
grep -o 'riscv,isa = "[^"]*"' $O/rocket.dts | head -1
ls $O | wc -l
echo "GEN_DONE $CFG"
