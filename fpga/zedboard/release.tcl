# Rocket out of reset (after load.tcl with ZED_NORELEASE)
if {[info exists ::env(ZED_HWSERVER)]} { connect -url $::env(ZED_HWSERVER) } else { connect }
targets -set -nocase -filter {name =~ "ARM*#0"}
stop
mwr 0xE000A048 0x1
con
puts "ZED_RELEASED"
