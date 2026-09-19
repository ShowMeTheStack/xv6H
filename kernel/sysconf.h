// sysconf(name): the values a program can ask the kernel about.
#define _SC_PAGESIZE          1
#define _SC_PHYS_PAGES        2   // frames the allocator manages
#define _SC_AVPHYS_PAGES      3   // free now
#define _SC_NPROCESSORS_ONLN  4
#define _SC_CLK_TCK           5   // ticks per second
#define _SC_XV6_MAXVA       100   // one past the highest user virtual address
#define _SC_XV6_TRAPFRAME   101   // the trapframe page (user memory ends below it)
#define _SC_XV6_MMU_LEVELS  102   // 4 (Sv48) or 3 (Sv39)
