#define ARG_MAX     (16 * 4096)       // bytes of arguments exec accepts
// timer interrupts a second, which is the resolution of every timeout
// the kernel counts in ticks and the rate at which the scheduler gets
// to look at what is running. Ten was xv6's; a hundred is Linux's,
// and a scheduler whose slices are three and twenty-four milliseconds
// cannot mean anything if it is only asked once every hundred.
//
// this is the default. The rate is a variable (trap.c), and
// /sys/kernel/tick_hz changes it while the machine runs.
#define TICK_HZ     100               // timer ticks per second
#define PID_MAX     4194304           // pids are allocated below this (pidmax=N on the command line)
#define KSTACK_PAGES 8                // kernel stack per process: 32 KB, plus a guard page
#define NCPU        8                 // maximum number of CPUs
#define NSCHEDGROUP 64                // scheduler groups: VMs, containers, cgroups
#define SCHED_GROUP_NAMELEN 16
#define NOFILE      1024              // open files per process, at most
#define NOFILE_INIT 64                // ... initially (the table grows on demand)
// (open files come from an object cache; there is no system-wide limit)
#define NINODE      50                // maximum number of active i-nodes
#define NDEV        12                // maximum major device number
#define MAXARG      32                // max exec arguments
#define MAXENV      64                // max environment entries
#define FILEWRITE_MAX (256 * 1024)     // bytes one write(2) hands the file system at a time
#define FSSIZE      4000              // size of file system in blocks
// the longest path a system call may be given. a single name may be 255
// bytes (NAME_MAX in fs.h), so 128 was shorter than one name: this is
// four of them and a little, and it is a kernel stack buffer in every
// call that takes a path, against a 32 KB stack.
#define MAXPATH     1024
#define USERSTACK   8                 // user stack pages (TLS needs a few)
#define NVIRTIO     8                 // virtio-mmio slots on qemu virt
#ifndef HOSTMEM_MB
#define HOSTMEM_MB  128               // RAM the host kernel keeps for itself
#endif
#define HOSTMEM     ((uint64)HOSTMEM_MB * 1024 * 1024)
#define NVM         64                // VM slots (a VM's memory is allocated when the slot is used)
#define NVCPU       64                // maximum vcpus per VM (more than harts is fine: they are time-sliced)
#define XV6H_RELEASE "1.0"
#define XV6H_VERSION "xv6H riscv64"
