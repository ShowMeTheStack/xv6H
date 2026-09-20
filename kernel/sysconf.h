#ifndef XV6_SYSCONF_H
#define XV6_SYSCONF_H
// sysconf(name): the values a program can ask the kernel about.
#define _SC_PAGESIZE          1
#define _SC_PHYS_PAGES        2   // frames the allocator manages
#define _SC_AVPHYS_PAGES      3   // free now
#define _SC_NPROCESSORS_ONLN  4
#define _SC_CLK_TCK           5   // ticks per second
#define _SC_XV6_MAXVA       100   // one past the highest user virtual address
#define _SC_XV6_TRAPFRAME   101   // the trapframe page (user memory ends below it)
#define _SC_XV6_MMU_LEVELS  102   // 4 (Sv48) or 3 (Sv39)
#define _SC_XV6_NPROCS      103   // processes that exist right now
#define _SC_XV6_PID_MAX     104
#define _SC_XV6_BRK_MAX     105   // the highest address the heap (sbrk) may reach
#define _SC_XV6_STACK_MAX   106   // the stack region's size
#define _SC_XV6_STACK_GUARD 107   // the unmapped page below the stack
#define _SC_XV6_CACHEPAGES  108   // pages the block cache holds and would give back

#endif
