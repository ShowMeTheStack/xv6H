//
// a virtual goldfish RTC for guests: the host's clock, at the same
// address qemu's virt machine uses, so a guest Linux finds it with its
// stock driver and a guest xv6 with rtc.c. reads only; the alarm and
// its interrupt are not modelled (the interrupt line never fires).
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "hyp.h"

static int
vrtc_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  uint64 v = 0;
  switch (off) {
  case 0x00: // TIME_LOW: latches the high word
    vm->rtc_latch = clock_realtime_ns();
    v = (uint32)vm->rtc_latch;
    break;
  case 0x04: // TIME_HIGH
    v = vm->rtc_latch >> 32;
    break;
  default:   // ALARM_LOW/HIGH, IRQ_ENABLED, ALARM_STATUS: nothing pending
    v = 0;
    break;
  }
  *val = v;
  return 0;
}

static int
vrtc_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  return 0; // the clock is the host's; alarms and time writes are ignored
}

void
vrtc_init(struct vm *vm)
{
  vm->rtc_latch = 0;
  if (rtc_present())
    vm_add_dev(vm, "rtc", G_RTC_IRQ, OBS_RTC, G_RTC_BASE, 0x1000, vrtc_read, vrtc_write, 0);
}
