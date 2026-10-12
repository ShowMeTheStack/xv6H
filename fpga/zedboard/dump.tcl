# The RAM disk read out of DDR over JTAG, Rocket left running (the read goes through A9 #1)
#   xsdb dump.tcl <out file> <DDR address> <bytes>
set out [lindex $argv 0]
set addr [lindex $argv 1]
set bytes [lindex $argv 2]
if {[info exists ::env(ZED_HWSERVER)]} { connect -url $::env(ZED_HWSERVER) } else { connect }
targets -set -nocase -filter {name =~ "ARM*#1"}
stop
set t0 [clock milliseconds]
mrd -bin -file $out $addr [expr {$bytes / 4}]
puts "DUMPED $bytes bytes in [expr {[clock milliseconds] - $t0}] ms"
con
