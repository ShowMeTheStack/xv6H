#!/bin/bash
# B.2 (runs on the build box): the board from nothing, with the ILA watching Rocket's first moves.
#   bash debug.sh <images dir> <build dir>   -> <build dir>/ila.csv and the console in zedb-console.log
# One container, one hw_server: xsdb (the load, the release) and Vivado (the ILA) share it.
set -e
I=${1:-$HOME/xv6h-fpga/zed-images}; B=${2:-$HOME/xv6h-fpga/zed-XV6HZedSoCConfig}; F=$HOME/xv6h-fpga; Z=$F/zedboard
docker run --rm --privileged -e ZED_ILA_AT=${ZED_ILA_AT:-} -v /dev/bus/usb:/dev/bus/usb -v $F:$F kkrizka/vivado:2020.1 bash -lc "
  source /opt/Xilinx/Vivado/2020.1/settings64.sh
  hw_server -d -s tcp::3121 >/dev/null 2>&1; sleep 3
  export ZED_ILA_AT=${ZED_ILA_AT:-}; [ -z \"\$ZED_ILA_AT\" ] && unset ZED_ILA_AT
  export ZED_HWSERVER=tcp:127.0.0.1:3121 ZED_HWSERVER_VIVADO=127.0.0.1:3121 ZED_NORELEASE=1
  xsdb $Z/load.tcl $I $I/bridge.elf $I/kernel.bin 0x10000000 $I/zed.dtb 0x1fe00000 $I/fs.img 0x1a000000
  unset ZED_NORELEASE
  cp $I/xv6hzed.ltx $B/ 2>/dev/null || true
  vivado -mode batch -nojournal -nolog -source $Z/ila.tcl -tclargs $B $B/ila.csv $Z/release.tcl
"
