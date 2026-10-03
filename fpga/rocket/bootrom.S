// xv6H on the ZedBoard: the Rocket boot ROM. At reset, jump straight to the kernel in DDR with
// a0 = hart ID and a1 = the device tree the ARM loader placed at DTB_ADDR (it describes the shell:
// the UART, the PLIC lines and the RAM disk, which Rocket's own generated tree does not).
#define DRAM_BASE 0x80000000
#define DTB_ADDR  0x8fe00000
.section .text.start, "ax", @progbits
.globl _start
_start:
  csrwi 0x7c1, 0
  csrr a0, mhartid
  li a1, DTB_ADDR
  li s0, DRAM_BASE
  jr s0
