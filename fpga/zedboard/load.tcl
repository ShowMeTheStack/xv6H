# xv6H on the ZedBoard: bring the board up from nothing, over JTAG.
#   xsdb load.tcl <dir with xv6hzed.bit and ps7_init.tcl> <bridge.elf> <image> <addr> [<image> <addr> ...]
# Addresses are DDR addresses as the ARM sees them; Rocket's 0x8xxx_xxxx is DDR 0x1xxx_xxxx.
# Order: the PS first (clocks, DDR, MIO), the PL programmed, the images loaded with Rocket
# held in reset (GPIO EMIO 0 low), the bridge started, and then Rocket let go.
set d [lindex $argv 0]
set bridge [lindex $argv 1]
# ZED_HWSERVER=tcp:host:port: use a running hw_server (debug.sh shares one with Vivado's ILA)
if {[info exists ::env(ZED_HWSERVER)]} { connect -url $::env(ZED_HWSERVER) } else { connect }
targets -set -nocase -filter {name =~ "APU*"}
rst -system
after 1000
targets -set -nocase -filter {name =~ "ARM*#0"}
rst -processor
source $d/ps7_init.tcl
ps7_init
fpga -file $d/xv6hzed.bit
ps7_post_config
# GPIO bank 2 (EMIO 54..): pin 0 an output, low -- Rocket in reset
mwr 0xE000A284 0x1
mwr 0xE000A288 0x1
mwr 0xE000A048 0x0
foreach {img addr} [lrange $argv 2 end] {
  puts "loading $img at $addr"
  dow -data $img $addr
}
dow $bridge
con
after 500
# ZED_NORELEASE: leave Rocket in reset (release.tcl lets it go, once an ILA is armed)
if {[info exists ::env(ZED_NORELEASE)]} { puts "ZED_LOADED"; exit }
# Rocket out of reset: the GPIO write goes through the APU's memory port while the bridge runs
stop
mwr 0xE000A048 0x1
con
puts "ZED_RUNNING"
