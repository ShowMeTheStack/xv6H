#!/usr/bin/env python3
# The device tree xv6H boots with on the ZedBoard (the ARM loader puts it at Rocket's
# 0x8fe0_0000, where the boot ROM's a1 points), and its QEMU stand-in with the board's ISA
# profile. Two address cells and two size cells (xv6H's parser; Rocket's own generated tree
# has one). Rocket's map: CLINT 0x0200_0000, PLIC 0x0c00_0000 (contexts M then S of hart 0,
# four sources), the PL 16550 at 0x1000_0000 on source 1; RAM 0x8000_0000 + 160 MB (48 kept
# by the host); the RAM disk after it, outside the memory node.
#   mkdts.py board|qemu > x.dts
import sys
which = sys.argv[1]
import os
# ZED_BOOTARGS adds words (`uartdebug': the bring-up heartbeat)
bootargs = ("hostmem=48 " + os.environ.get("ZED_BOOTARGS", "")).strip()
board = which == "board"
# mtime ticks once per 100 cycles of the 40 MHz clock (the CLINT's divider was set for a
# 100 MHz bus): 400 kHz on the board. QEMU's virt counts at 10 MHz.
timebase = 400000 if board else 10000000
uart_irq = 1 if board else 10
plic_ndev = 4 if board else 95
uart_clock = 40000000 if board else 3686400
RAM, RAM_MB = 0x80000000, 160
RD, RD_SIZE = 0x8a000000, 0x5e00000      # 94 MB, up to the DT at 0x8fe0_0000
def cells(x): return "0x%x 0x%x" % (x >> 32, x & 0xffffffff)
print(f"""/dts-v1/;
/ {{
	#address-cells = <2>;
	#size-cells = <2>;
	compatible = "xv6h,zedboard-rocket";
	model = "xv6H on Rocket + H, ZedBoard ({which})";
	chosen {{
		bootargs = "{bootargs}";
		stdout-path = "/soc/serial@10000000";
	}};
	cpus {{
		#address-cells = <1>;
		#size-cells = <0>;
		timebase-frequency = <{timebase}>;
		cpu0: cpu@0 {{
			device_type = "cpu";
			compatible = "sifive,rocket0", "riscv";
			reg = <0>;
			status = "okay";
			riscv,isa = "rv64imafdch_zicsr_zifencei_zihpm";
			mmu-type = "riscv,sv39";
			cpu0_intc: interrupt-controller {{
				#interrupt-cells = <1>;
				compatible = "riscv,cpu-intc";
				interrupt-controller;
			}};
		}};
	}};
	memory@80000000 {{
		device_type = "memory";
		reg = <{cells(RAM)} {cells(RAM_MB << 20)}>;
	}};
	soc {{
		#address-cells = <2>;
		#size-cells = <2>;
		compatible = "simple-bus";
		ranges;
		clint@2000000 {{
			compatible = "riscv,clint0";
			interrupts-extended = <&cpu0_intc 3 &cpu0_intc 7>;
			reg = <0x0 0x2000000 0x0 0x10000>;
		}};
		plic: interrupt-controller@c000000 {{
			#interrupt-cells = <1>;
			#address-cells = <0>;
			compatible = "sifive,plic-1.0.0", "riscv,plic0";
			interrupt-controller;
			interrupts-extended = <&cpu0_intc 11 &cpu0_intc 9>;
			reg = <0x0 0xc000000 0x0 0x4000000>;
			riscv,ndev = <{plic_ndev}>;
		}};
		serial@10000000 {{
			compatible = "ns16550a";
			reg = <0x0 0x10000000 0x0 0x100>;
			interrupt-parent = <&plic>;
			interrupts = <{uart_irq}>;
			clock-frequency = <{uart_clock}>;
		}};
		ramdisk@{RD:x} {{
			compatible = "xv6h,ramdisk";
			reg = <{cells(RD)} {cells(RD_SIZE)}>;
		}};
	}};
}};""")
