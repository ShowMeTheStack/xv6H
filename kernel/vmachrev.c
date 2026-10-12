//
// The anchor. A revision changes the machine's description, and the guest
// has to be told; the telling has to arrive over something the revision is
// not changing. For xv6 guests that is the supervisor software interrupt,
// which is architectural and has no address. Linux cannot spare its SSIP
// (it is the SBI IPI, and a stray one is silently eaten by the IPI mux), so
// the machine has one device that is promised never to move: a page of
// registers at G_MACHREV_BASE with line G_MACHREV_IRQ, outside the F2/F3
// shifts. The line goes up when a revision is asked for; the driver lowers
// it (ACK register), reads the rest through the SBI MACH extension exactly
// as an xv6 guest does, and acknowledges through SBI. The registers only
// repeat what SBI answers, for a driver that would rather not ecall.
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

#define MR_STATUS  0x00   // bit 0: a revision is pending (asked, not adopted)
#define MR_ACK     0x04   // write anything: lower the line
#define MR_GEN     0x08   // the revision generation
#define MR_WHAT    0x0c   // what the last revision changed (REV_* bits)
#define MR_DTB_LO  0x10   // where the description is
#define MR_DTB_HI  0x14
#define MR_DTB_LEN 0x18
#define MR_VERSION 0x1c

static int
mr_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  uint64 v = 0;
  switch (off) {
  case MR_STATUS:  v = vm->revise_acked != vm->revise_gen; break;
  case MR_GEN:     v = vm->revise_gen; break;
  case MR_WHAT:    v = vm->revise_what; break;
  case MR_DTB_LO:  v = (uint32)vm->desc_gpa; break;
  case MR_DTB_HI:  v = vm->desc_gpa >> 32; break;
  case MR_DTB_LEN: v = VM_DTB_MAX; break;
  case MR_VERSION: v = 1; break;
  }
  *val = v;
  return 0;
}

static int
mr_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  if (off == MR_ACK) {
    acquire(&vm->lock);
    if (vm->machrev_line) {
      vm->machrev_line = 0;
      vic_set_level(vm, G_MACHREV_IRQ, 0);
    }
    release(&vm->lock);
  }
  return 0;
}

void
vmachrev_init(struct vm *vm)
{
  vm->machrev_line = 0;
  // irq -1: the line is driven here, by its fixed number, not through the
  // device table's F3 mapping -- this is the one line that map must not touch
  vm_add_dev(vm, "machrev", -1, 0, G_MACHREV_BASE, 0x1000, mr_read, mr_write, 0);
}

// a revision was asked for. caller holds vm->lock.
void
vmachrev_notify(struct vm *vm)
{
  // Every announcement is an edge. A line already high (an announcement the guest has not yet
  // acknowledged on the device, or a machine restored with the line high in its controller's
  // state) raised again was no change at all: in the APLIC's MSI mode no message was sent, so a
  // guest told only by this line -- a Linux bound to its IMSIC, which never enables the
  // software interrupt -- was never told. Found when a Linux saved on QEMU was restored on the
  // ZedBoard and on its QEMU stand-in: the revision on arrival was "described no, repeated 10
  // times" with the anchor's level held.
  if (vm->machrev_line)
    vic_set_level(vm, G_MACHREV_IRQ, 0);
  vm->machrev_line = 1;
  vic_set_level(vm, G_MACHREV_IRQ, 1);
}
