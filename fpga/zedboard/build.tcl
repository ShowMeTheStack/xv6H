# xv6H on the ZedBoard: the Zynq shell around Rocket + H.
#
#   PS7 (ZedBoard preset: DDR, MIO, UART1 on the USB-UART)
#     FCLK_CLK0 40 MHz -> everything in the PL
#     S_AXI_HP0 (64-bit) <- Rocket's memory port (remapped to DDR 0x1000_0000.., rocket_wrap.v)
#     UART0 on EMIO     <-> the PL 16550 (the ARM bridges UART0 and UART1)
#     GPIO EMIO[0]      -> Rocket held in reset until the ARM has loaded DDR
#   rocket_wrap (Rocket + H, generated)
#     m_mmio_axi -> AXI interconnect -> uart16550_axil at 0x1000_0000
#     irq[0]     <- the UART's interrupt
#
#   vivado -mode batch -source build.tcl -tclargs <rtl dir> <out dir>
set rtl [lindex $argv 0]
set out [lindex $argv 1]
set here [file dirname [info script]]
file mkdir $out
create_project -force xv6hzed $out/proj -part xc7z020clg484-1
set_property board_part avnet.com:zedboard:part0:1.4 [current_project]

# sources: the generated Rocket, its block-RAM macros, the wrapper and the UART
foreach f [glob -nocomplain $rtl/*.sv] { add_files -norecurse $f }
foreach f [glob -nocomplain $rtl/*.v] { add_files -norecurse $f }
add_files -norecurse $here/rocket_wrap.v
add_files -norecurse $here/uart16550_axil.v
set_property file_type SystemVerilog [get_files *.sv]
update_compile_order -fileset sources_1

create_bd_design "zed"
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7]
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
  -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} $ps
set_property -dict [list \
  CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {40} \
  CONFIG.PCW_USE_S_AXI_HP0 {1} \
  CONFIG.PCW_USE_M_AXI_GP0 {0} \
  CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} \
  CONFIG.PCW_UART0_PERIPHERAL_ENABLE {1} \
  CONFIG.PCW_UART0_UART0_IO {EMIO} \
  CONFIG.PCW_GPIO_EMIO_GPIO_ENABLE {1} \
  CONFIG.PCW_GPIO_EMIO_GPIO_IO {1} \
] $ps

set rk [create_bd_cell -type module -reference rocket_wrap rocket]
set ua [create_bd_cell -type module -reference uart16550_axil uart]
set rst [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst]
set icm [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 icm_mem]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] $icm
set icd [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 icm_dev]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] $icd
set gate [create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 rstgate]
set_property -dict [list CONFIG.C_SIZE {1} CONFIG.C_OPERATION {and}] $gate
set cat [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 irqs]
set_property -dict [list CONFIG.NUM_PORTS {2} CONFIG.IN0_WIDTH {1} CONFIG.IN1_WIDTH {3}] $cat
set zero [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 zero3]
set_property -dict [list CONFIG.CONST_WIDTH {3} CONFIG.CONST_VAL {0}] $zero

# clocks and resets: everything on FCLK_CLK0; Rocket's reset also waits for the ARM's GPIO
set clk [get_bd_pins ps7/FCLK_CLK0]
foreach p {rocket/clk uart/s_axi_aclk rst/slowest_sync_clk ps7/S_AXI_HP0_ACLK
           icm_mem/ACLK icm_mem/S00_ACLK icm_mem/M00_ACLK
           icm_dev/ACLK icm_dev/S00_ACLK icm_dev/M00_ACLK} {
  connect_bd_net $clk [get_bd_pins $p]
}
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rst/ext_reset_in]
connect_bd_net [get_bd_pins rst/peripheral_aresetn] [get_bd_pins rstgate/Op1]
connect_bd_net [get_bd_pins ps7/GPIO_O] [get_bd_pins rstgate/Op2]
connect_bd_net [get_bd_pins rstgate/Res] [get_bd_pins rocket/resetn]
foreach p {uart/s_axi_aresetn icm_mem/ARESETN icm_mem/S00_ARESETN icm_mem/M00_ARESETN
           icm_dev/ARESETN icm_dev/S00_ARESETN icm_dev/M00_ARESETN} {
  connect_bd_net [get_bd_pins rst/peripheral_aresetn] [get_bd_pins $p]
}

# memory: Rocket -> HP0 (DDR)
connect_bd_intf_net [get_bd_intf_pins rocket/m_mem_axi] [get_bd_intf_pins icm_mem/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins icm_mem/M00_AXI] [get_bd_intf_pins ps7/S_AXI_HP0]
# devices: Rocket's MMIO port reaches the UART. (The ARM's GP0 did too, at another address: one
# slave at two addresses on one crossbar is illegal, BD 41-1353. The ARM talks to the UART over
# its own UART0 on EMIO and never needed GP0.)
connect_bd_intf_net [get_bd_intf_pins rocket/m_mmio_axi] [get_bd_intf_pins icm_dev/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins icm_dev/M00_AXI] [get_bd_intf_pins uart/s_axi]

# the UART: its lines to the PS's UART0 over EMIO, its interrupt to Rocket's irq[0]
connect_bd_net [get_bd_pins uart/tx] [get_bd_pins ps7/UART0_RX]
connect_bd_net [get_bd_pins ps7/UART0_TX] [get_bd_pins uart/rx]
connect_bd_net [get_bd_pins uart/irq] [get_bd_pins irqs/In0]
connect_bd_net [get_bd_pins zero3/dout] [get_bd_pins irqs/In1]
connect_bd_net [get_bd_pins irqs/dout] [get_bd_pins rocket/irq]

# addresses: Rocket's memory port reaches the DDR through HP0 (remapped in rocket_wrap.v);
# the UART at 0x1000_0000 in Rocket's device window
assign_bd_address
# the DDR segment is fixed at 0 [512M]; the wrapper's 0x1xxx_xxxx addresses fall in its upper half
foreach seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces rocket/m_mmio_axi]] {
  set_property offset 0x10000000 $seg; set_property range 4K $seg
}
foreach sp {rocket/m_mem_axi rocket/m_mmio_axi} {
  foreach seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces $sp]] {
    puts "ADDR $sp [get_property NAME $seg] [get_property OFFSET $seg] [get_property RANGE $seg]"
  }
}
validate_bd_design
save_bd_design
set w [make_wrapper -files [get_files zed.bd] -top]
add_files -norecurse $w
set_property top zed_wrapper [current_fileset]
update_compile_order -fileset sources_1

# out-of-band timing: 40 MHz is the target
launch_runs synth_1 -jobs 8
wait_on_run synth_1

# an ILA on Rocket's reset, memory reads and device writes (the board is not a black box):
# captured over JTAG by fpga/zedboard/ila.tcl
open_run synth_1 -name synth_1
create_debug_core u_ila_0 ila
set_property C_DATA_DEPTH 4096 [get_debug_cores u_ila_0]
set_property C_TRIGIN_EN false [get_debug_cores u_ila_0]
set_property C_INPUT_PIPE_STAGES 1 [get_debug_cores u_ila_0]
set_property port_width 1 [get_debug_ports u_ila_0/clk]
connect_debug_port u_ila_0/clk [get_nets zed_i/ps7_FCLK_CLK0]
set first 1
foreach pat {zed_i/rstgate_Res* zed_i/rocket_m_mem_axi_ARVALID zed_i/rocket_m_mem_axi_ARREADY zed_i/rocket_m_mem_axi_ARADDR*
             zed_i/rocket_m_mem_axi_RVALID zed_i/rocket_m_mem_axi_RREADY zed_i/rocket_m_mem_axi_RRESP* zed_i/rocket_m_mem_axi_RLAST
             zed_i/rocket_m_mem_axi_AWVALID zed_i/rocket_m_mem_axi_BRESP* zed_i/rocket_m_mem_axi_BVALID
             zed_i/rocket_m_mmio_axi_AWVALID zed_i/rocket_m_mmio_axi_AWREADY zed_i/rocket_m_mmio_axi_AWADDR* zed_i/rocket_m_mmio_axi_WDATA*
             zed_i/rocket_m_mmio_axi_WVALID zed_i/rocket_m_mmio_axi_BVALID zed_i/rocket_m_mmio_axi_BRESP*} {
  set nets [lsort -dictionary [get_nets -quiet $pat]]
  if {[llength $nets] == 0} { puts "ILA: no net $pat"; continue }
  if {!$first} { create_debug_port u_ila_0 probe }
  set p [lindex [get_debug_ports u_ila_0/probe*] end]
  set_property port_width [llength $nets] $p
  connect_debug_port $p $nets
  puts "ILA: $p <- $pat ([llength $nets])"
  set first 0
}
# the debug core goes into the project's constraints, and the implementation run inserts it
file mkdir $out/xdc
close [open $out/xdc/ila.xdc w]
add_files -fileset constrs_1 -norecurse $out/xdc/ila.xdc
set_property target_constrs_file $out/xdc/ila.xdc [get_filesets constrs_1]
save_constraints -force
close_design
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
open_run impl_1
report_utilization -file $out/util.rpt
report_timing_summary -file $out/timing.rpt
write_hw_platform -fixed -include_bit -force -file $out/xv6hzed.xsa
file copy -force [glob $out/proj/xv6hzed.runs/impl_1/*.bit] $out/xv6hzed.bit
write_debug_probes -force $out/xv6hzed.ltx
puts "BUILD_DONE"
