# The ILA on Rocket's ports (build.tcl), captured over JTAG.
#   vivado -mode batch -source ila.tcl -tclargs <build dir> <out.csv> <release.tcl>
# Armed on Rocket's reset going high, Rocket released (xsdb release.tcl), 4096 samples written.
set d [lindex $argv 0]
set csv [lindex $argv 1]
set rel [lindex $argv 2]
open_hw_manager
connect_hw_server -url $::env(ZED_HWSERVER_VIVADO)
open_hw_target
set dev [lindex [get_hw_devices xc7z020*] 0]
current_hw_device $dev
set_property PROBES.FILE $d/xv6hzed.ltx $dev
refresh_hw_device $dev
set ila [lindex [get_hw_ilas -of_objects $dev] 0]
puts "ILA [get_property NAME $ila]: [llength [get_hw_probes -of_objects $ila]] probes"
set_property CONTROL.TRIGGER_POSITION 64 $ila
# ZED_ILA_AT=0x1xxxxxxx: trigger on Rocket fetching that line (DDR address) instead of its reset
if {[info exists ::env(ZED_ILA_AT)]} {
  set_property CONTROL.TRIGGER_POSITION 16 $ila
  set_property TRIGGER_COMPARE_VALUE eq1'b1 [lindex [get_hw_probes -of_objects $ila -filter {NAME =~ *mem_axi_ARVALID*}] 0]
  set_property TRIGGER_COMPARE_VALUE eq32'h[format %08X $::env(ZED_ILA_AT)] [lindex [get_hw_probes -of_objects $ila -filter {NAME =~ *mem_axi_ARADDR*}] 0]
  puts "TRIGGER at fetch of $::env(ZED_ILA_AT)"
} else {
  set rst [lindex [get_hw_probes -of_objects $ila -filter {NAME =~ *rstgate_Res*}] 0]
  set_property TRIGGER_COMPARE_VALUE eq1'bR $rst
}
run_hw_ila $ila
after 1000
puts "ARMED; releasing Rocket"
puts [exec xsdb $rel]
wait_on_hw_ila -timeout 2 $ila
write_hw_ila_data -force -csv_file $csv [upload_hw_ila_data $ila]
puts "ILA_WRITTEN $csv"
