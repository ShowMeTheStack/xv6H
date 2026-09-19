//
// the real-time clock: qemu's virt machine carries a goldfish RTC (a
// 64-bit count of nanoseconds since the unix epoch, read low word first,
// which latches the high word), and every guest of ours gets a virtual
// one at the same address (vrtc.c). the monotonic clock is the RISC-V
// time counter scaled by the device tree's timebase-frequency.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "platform.h"
#include "time.h"

#define RTC_TIME_LOW  0x00
#define RTC_TIME_HIGH 0x04

static struct spinlock rtclock;

void
rtcinit(void)
{
  initlock(&rtclock, "rtc");
  if (plat.rtc_base == 0) {
    printk("rtc: none; CLOCK_REALTIME unavailable\n");
    return;
  }
  uint64 ns = clock_realtime_ns();
  printk("rtc: goldfish at 0x%lx, epoch seconds %ld\n", plat.rtc_base, ns / 1000000000UL);
}

int
rtc_present(void)
{
  return plat.rtc_base != 0;
}

// nanoseconds since the epoch, or 0 without an RTC.
uint64
clock_realtime_ns(void)
{
  if (plat.rtc_base == 0)
    return 0;
  // the two reads must not be interleaved with another hart's.
  acquire(&rtclock);
  uint32 lo = *(volatile uint32 *)(plat.rtc_base + RTC_TIME_LOW);
  uint32 hi = *(volatile uint32 *)(plat.rtc_base + RTC_TIME_HIGH);
  release(&rtclock);
  return ((uint64)hi << 32) | lo;
}

// nanoseconds since boot.
uint64
clock_monotonic_ns(void)
{
  uint64 t = r_time(), tb = plat.timebase ? plat.timebase : 10000000;
  return (t / tb) * 1000000000UL + (t % tb) * 1000000000UL / tb;
}
