#ifndef XV6_MEMLAYOUT_H
#define XV6_MEMLAYOUT_H
// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio disk
// 80000000 -- qemu's boot ROM loads the kernel here,
//             then jumps here.
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0     0x10000000L
#define UART0_IRQ 10

// virtio mmio interface
#define VIRTIO0     0x10001000
#define VIRTIO0_IRQ 1

// core-local interrupt controller (CLINT)
#define CLINT_BASE  0x02000000L
#define CLINT(hart) (CLINT_BASE + (hart) * 4)

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC                 0x0c000000L
#define PLIC_PRIORITY        (PLIC + 0x0)
#define PLIC_PENDING         (PLIC + 0x1000)
#define PLIC_SENABLE(hart)   (PLIC + 0x2080 + (hart) * 0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart)    (PLIC + 0x201004 + (hart) * 0x2000)

// qemu's AIA interrupt controllers (-machine virt,aia=aplic-imsic).
#define APLIC_M   0x0c000000L
#define APLIC_S   0x0d000000L
#define IMSIC_M   0x24000000L
#define IMSIC_S   0x28000000L

// the kernel expects there to be RAM for use by the kernel and user
// pages from physical address 0x80000000 to the page database at the top of RAM (the device
// tree says how much RAM there is; see platform.c). PHYSTOP is only
// the size a guest gets by default.
#define KERNBASE 0x80000000L
#define PHYSTOP  (KERNBASE + 128 * 1024 * 1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
// the paging mode is chosen at boot (Sv48 or Sv39), so the top of the
// address space, and with it the trampoline, trapframe and kernel stack
// addresses, are runtime values (plat.maxva). kernel code only.
#ifdef KERNEL
#define TRAMPOLINE (plat.maxva - PGSIZE)
#endif

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#ifdef KERNEL
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)
#endif

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#ifdef KERNEL
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
// the page holding the signal-return trampoline, readable and executable
// by the process (the trampoline page itself is not).
#define SIGTRAMP (TRAPFRAME - PGSIZE)

// every thread needs a trapframe at its own address in the shared
// address space: they are mapped downward from here.
#define THREAD_TF_SLOTS 1024
#define THREAD_TF_TOP   (SIGTRAMP - PGSIZE)
#define THREAD_TF_BOT   (THREAD_TF_TOP - THREAD_TF_SLOTS * PGSIZE)
#endif
// qemu's virt board puts the "sifive,test" finisher here: a halfword
// written to it ends the machine, 0x5555 for a power off and 0x7777
// for a reset. a real board would use SBI SRST instead; this kernel is
// the one under SBI.
#define SIFIVE_TEST 0x100000L

#endif
