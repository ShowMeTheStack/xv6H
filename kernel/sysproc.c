#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "platform.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "time.h"
#include "page.h"
#include "sysconf.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// clock_gettime(clock, &timespec): CLOCK_REALTIME from the RTC (fails
// without one), CLOCK_MONOTONIC from the time counter.
uint64
sys_clock_gettime(void)
{
  int clk;
  uint64 addr, ns;
  struct timespec ts;

  argint(0, &clk);
  argaddr(1, &addr);
  if (clk == CLOCK_REALTIME) {
    if (!rtc_present())
      return -1;
    ns = clock_realtime_ns();
  } else if (clk == CLOCK_MONOTONIC) {
    ns = clock_monotonic_ns();
  } else {
    return -1;
  }
  ts.sec = ns / 1000000000UL;
  ts.nsec = ns % 1000000000UL;
  if (either_copyout(1, addr, &ts, sizeof(ts)) < 0)
    return -1;
  return 0;
}

// sysconf(name): see sysconf.h. -1 for an unknown name.
uint64
sys_sysconf(void)
{
  int name;
  argint(0, &name);
  switch (name) {
  case _SC_PAGESIZE: return PGSIZE;
  case _SC_PHYS_PAGES: {
    struct meminfo mi;
    kmem_info(&mi);
    return mi.total_pages;
  }
  case _SC_AVPHYS_PAGES: return kmem_free_pages();
  case _SC_NPROCESSORS_ONLN: return plat.ncpu;
  case _SC_CLK_TCK: return 10;
  case _SC_XV6_MAXVA: return plat.maxva;
  case _SC_XV6_TRAPFRAME: return TRAPFRAME;
  case _SC_XV6_MMU_LEVELS: return plat.mmu_levels;
  }
  return -1;
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
