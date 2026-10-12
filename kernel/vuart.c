//
// virtual 16550 UART: the guest's console.
//
// transmitted bytes go into the VM's cons_out ring, which a host process
// reads through the vm_cons system call; bytes the host writes land in
// cons_in and appear as received characters, with a receive interrupt.
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

#define RHR 0
#define THR 0
#define IER 1
#define ISR 2
#define FCR 2
#define LCR 3
#define MCR 4
#define LSR 5
#define MSR 6
#define SCR 7

#define IER_RX 1
#define IER_TX 2
#define LCR_DLAB 0x80
#define LSR_DR 0x01
#define LSR_THRE 0x20
#define LSR_TEMT 0x40

int
vuart_irq_level(struct vm *vm)
{
  struct vuart *u = &vm->uart;
  if ((u->ier & IER_RX) && !cring_empty(&vm->cons_in))
    return 1;
  if ((u->ier & IER_TX) && u->thre_intr)
    return 1;
  return 0;
}

// caller holds vm->lock.
static void
update(struct vm *vm)
{
  vic_set_level(vm, vm_irq(vm, G_UART_IRQ), vuart_irq_level(vm));
}

// caller holds vm->lock. blocks (dropping the lock) while a host
// reader is attached and the ring is full; otherwise overwrites the
// oldest output.
static void
tx_put(struct vm *vm, char c)
{
  while (cring_full(&vm->cons_out) && vm->cons_attached) {
    struct proc *p = myproc();
    if (p == 0 || killed(p) || vm->state != VM_RUNNING)
      break;
    sleep_prepare(&vm->cons_out);
    release(&vm->lock);
    sleep_intr();
    acquire(&vm->lock);
  }
  if (cring_full(&vm->cons_out))
    vm->cons_out.r++; // drop oldest
  cring_put(&vm->cons_out, c);
  if (vm->revise_acked != vm->revise_gen) {   // revision trace: the guest's line before its ack
    if (!vm->t_first_tx) vm->t_first_tx = r_time();
    vm->t_last_tx = r_time();
    { struct proc *p = myproc(); vm->tx_vcpu = (p && p->vcpu) ? ((struct vcpu *)p->vcpu)->id : -1; }
  }
  wakeup(&vm->cons_out);
}

static int
vuart_read(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 *val)
{
  struct vuart *u = &vm->uart;
  uint64 v = 0;

  acquire(&vm->lock);
  switch (off) {
  case RHR:
    if (u->lcr & LCR_DLAB) {
      v = u->dll;
    } else if (!cring_empty(&vm->cons_in)) {
      v = (uint8)cring_get(&vm->cons_in);
      wakeup(&vm->cons_in);
    }
    break;
  case IER:
    v = (u->lcr & LCR_DLAB) ? u->dlm : u->ier;
    break;
  case ISR:
    if ((u->ier & IER_RX) && !cring_empty(&vm->cons_in)) {
      v = 0xc4; // received data available
    } else if ((u->ier & IER_TX) && u->thre_intr) {
      v = 0xc2; // transmitter holding register empty
      u->thre_intr = 0; // reading ISR clears the THRE indication
    } else {
      v = 0xc1; // no interrupt pending
    }
    break;
  case LCR:
    v = u->lcr;
    break;
  case MCR:
    v = u->mcr;
    break;
  case LSR:
    v = LSR_THRE | LSR_TEMT;
    if (!cring_empty(&vm->cons_in))
      v |= LSR_DR;
    break;
  case MSR:
    // loopback: modem outputs feed the inputs (CTS<-RTS, DSR<-DTR,
    // RI<-OUT1, DCD<-OUT2); otherwise report DSR and CTS asserted.
    if (u->mcr & 0x10)
      v = ((u->mcr & 1) << 5) | ((u->mcr & 2) << 3) | ((u->mcr & 4) << 4) | ((u->mcr & 8) << 4);
    else
      v = 0x30;
    break;
  case SCR:
    v = u->scr;
    break;
  }
  update(vm);
  release(&vm->lock);
  *val = v;
  return 0;
}

static int
vuart_write(struct vm *vm, struct vdev *d, uint64 off, int size, uint64 val)
{
  struct vuart *u = &vm->uart;

  acquire(&vm->lock);
  switch (off) {
  case THR:
    if (u->lcr & LCR_DLAB) {
      u->dll = val;
    } else {
      tx_put(vm, (char)val);
      if (u->ier & IER_TX)
        u->thre_intr = 1;
    }
    break;
  case IER:
    if (u->lcr & LCR_DLAB) {
      u->dlm = val;
    } else {
      u->ier = val & 0x0f;
      // enabling the transmit interrupt while THR is empty raises it.
      if (u->ier & IER_TX)
        u->thre_intr = 1;
    }
    break;
  case FCR:
    u->fcr = val;
    break;
  case LCR:
    u->lcr = val;
    break;
  case MCR:
    u->mcr = val;
    break;
  case SCR:
    u->scr = val;
    break;
  }
  update(vm);
  release(&vm->lock);
  return 0;
}

// the console is the guest's while a terminal is attached: the host's own
// printk lines are held meanwhile (printk.c, cons_owned). vm->lock held.
void
cons_set_attached(struct vm *vm, int on)
{
  if (vm->cons_attached == on)
    return;
  vm->cons_attached = on;
  __atomic_fetch_add(&cons_owned, on ? 1 : -1, __ATOMIC_SEQ_CST);
}

void
vuart_init(struct vm *vm)
{
  memset(&vm->uart, 0, sizeof(vm->uart));
  vm->cons_out.r = vm->cons_out.w = 0;
  vm->cons_in.r = vm->cons_in.w = 0;
  cons_set_attached(vm, 0);
  vm_add_dev(vm, "uart", G_UART_IRQ, OBS_UART, G_UART_BASE, 0x100, vuart_read, vuart_write, 0);
}

// what the virtio-console hands over: a whole descriptor's worth of
// output under one acquisition of the lock. tx_put may sleep while a host
// reader is attached and the ring is full, which is why this is here
// beside it rather than in vvcon.c reaching for a static.
void
vm_cons_emit(struct vm *vm, const char *buf, int n)
{
  acquire(&vm->lock);
  for (int i = 0; i < n; i++)
    tx_put(vm, buf[i]);
  release(&vm->lock);
}

// SBI console putchar.
void
vm_cons_putc(struct vm *vm, int c)
{
  acquire(&vm->lock);
  tx_put(vm, (char)c);
  release(&vm->lock);
}

// SBI console getchar: -1 if nothing is waiting.
int
vm_cons_getc(struct vm *vm)
{
  int c = -1;
  acquire(&vm->lock);
  if (!cring_empty(&vm->cons_in)) {
    c = (uint8)cring_get(&vm->cons_in);
    update(vm);
  }
  release(&vm->lock);
  return c;
}

// is there anything to read, or room to write? poll() and select()
// ask this of a machine's descriptor.
int
vm_cons_ready(struct vm *vm, int write)
{
  int r;

  acquire(&vm->lock);
  if (write)
    r = !cring_full(&vm->cons_in);
  else if (vm->state == VM_FREE || vm->cons_detach_req)
    r = -1;                 // a hangup: the machine is gone
  else
    r = cring_empty(&vm->cons_out) ? 0 : 1;
  release(&vm->lock);
  return r;
}

// host side: read up to n bytes of guest console output (blocking).
int
vm_cons_read(struct vm *vm, int user, uint64 dst, int n)
{
  char buf[128];
  int i = 0;

  if (n <= 0)
    return 0;
  if (n > sizeof(buf))
    n = sizeof(buf);

  acquire(&vm->lock);
  if (vm->cons_detach_req) {
    vm->cons_detach_req = 0;
    cons_set_attached(vm, 0);
    release(&vm->lock);
    printk_flush_held();
    return -1;
  }
  cons_set_attached(vm, 1);
  while (cring_empty(&vm->cons_out)) {
    if (killed(myproc()) || vm->state == VM_FREE || vm->cons_detach_req) {
      vm->cons_detach_req = 0;
      cons_set_attached(vm, 0);
      release(&vm->lock);
      printk_flush_held();
      return -1;
    }
    sleep_prepare(&vm->cons_out);
    release(&vm->lock);
    sleep_intr();
    acquire(&vm->lock);
  }
  while (i < n && !cring_empty(&vm->cons_out))
    buf[i++] = cring_get(&vm->cons_out);
  wakeup(&vm->cons_out); // a blocked guest writer may continue
  release(&vm->lock);

  if (either_copyout(user, dst, buf, i) < 0)
    return -1;
  return i;
}

// host side: type n bytes at the guest console.
int
vm_cons_write(struct vm *vm, int user, uint64 src, int n)
{
  char buf[128];
  int done = 0;

  while (done < n) {
    int k = n - done;
    if (k > sizeof(buf))
      k = sizeof(buf);
    if (either_copyin(buf, user, src + done, k) < 0)
      return -1;
    acquire(&vm->lock);
    for (int i = 0; i < k; i++) {
      if (cring_full(&vm->cons_in))
        break; // guest is not reading; drop
      cring_put(&vm->cons_in, buf[i]);
    }
    update(vm);
    release(&vm->lock);
    // and if the guest is reading its console through the queue rather
    // than the 16550, hand it over there too. Both devices draw from the
    // same ring, so whichever the guest drives is the one that sees it.
    // Not for a guest that boots its own harts (Linux): its login shell is
    // on the 16550 for good (console= is boot-time), while its hvc driver
    // posts buffers on the queue the moment the device appears -- and took
    // every keystroke (no prompt after `console virtio'). For it the
    // 16550 is the console anchor; the queue is an extra tty.
    if (!VM_FOREIGN(vm))
      vvcon_rx(vm);
    done += k;
  }
  return n;
}
