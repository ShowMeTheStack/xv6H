//
// formatted console output -- printk, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

volatile int panicking = 0; // printing a panic message
volatile int panicked = 0;  // spinning forever at end of a panic

// lock to avoid interleaving concurrent printk's.
static struct {
  struct spinlock lock;
  char buf[256]; // the message being formatted (under lock)
  int n;
} pr;

// append to the message; while panicking (no lock, other cpus may be
// in the middle of their own message) write straight through.
static void
pputc(int c)
{
  if (panicking) {
    consputc(c);
    return;
  }
  pr.buf[pr.n++] = c;
  if (pr.n == sizeof(pr.buf)) {
    consputs(pr.buf, pr.n);
    pr.n = 0;
  }
}

static void
pflush(void)
{
  if (pr.n > 0) {
    consputs(pr.buf, pr.n);
    // and into the ring /dev/kmsg reads, so that what the kernel said
    // is still there for klogd and for whoever asks later
    kmsg_note(pr.buf, pr.n);
    pr.n = 0;
  }
}

static char digits[] = "0123456789abcdef";

static void
printint(long long xx, int base, int sign)
{
  char buf[20];
  int i;
  unsigned long long x;

  if (sign && (sign = (xx < 0)))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);

  if (sign)
    buf[i++] = '-';

  while (--i >= 0)
    pputc(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  pputc('0');
  pputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    pputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console.
int
printk(char *fmt, ...)
{
  va_list ap;
  int i, cx, c0, c1, c2;
  char *s;

  if (panicking == 0)
    acquire(&pr.lock);

  va_start(ap, fmt);
  for (i = 0; (cx = fmt[i] & 0xff) != 0; i++) {
    if (cx != '%') {
      pputc(cx);
      continue;
    }
    i++;
    c0 = fmt[i + 0] & 0xff;
    c1 = c2 = 0;
    if (c0)
      c1 = fmt[i + 1] & 0xff;
    if (c1)
      c2 = fmt[i + 2] & 0xff;
    if (c0 == 'd') {
      printint(va_arg(ap, int), 10, 1);
    } else if (c0 == 'l' && c1 == 'd') {
      printint(va_arg(ap, uint64), 10, 1);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
      printint(va_arg(ap, uint64), 10, 1);
      i += 2;
    } else if (c0 == 'u') {
      printint(va_arg(ap, uint32), 10, 0);
    } else if (c0 == 'l' && c1 == 'u') {
      printint(va_arg(ap, uint64), 10, 0);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
      printint(va_arg(ap, uint64), 10, 0);
      i += 2;
    } else if (c0 == 'x') {
      printint(va_arg(ap, uint32), 16, 0);
    } else if (c0 == 'l' && c1 == 'x') {
      printint(va_arg(ap, uint64), 16, 0);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
      printint(va_arg(ap, uint64), 16, 0);
      i += 2;
    } else if (c0 == 'p') {
      printptr(va_arg(ap, uint64));
    } else if (c0 == 'c') {
      pputc(va_arg(ap, uint));
    } else if (c0 == 's') {
      if ((s = va_arg(ap, char *)) == 0)
        s = "(null)";
      for (; *s; s++)
        pputc(*s);
    } else if (c0 == '%') {
      pputc('%');
    } else if (c0 == 0) {
      break;
    } else {
      // Print unknown % sequence to draw attention.
      pputc('%');
      pputc(c0);
    }
  }
  va_end(ap);
  pflush();

  if (panicking == 0)
    release(&pr.lock);

  return 0;
}

// the return addresses on the stack, which the frame pointer chains
// together: the kernel is built with -fno-omit-frame-pointer, so s0
// points at the top of the current frame and the two words below it
// are the caller's return address and frame pointer.
void
backtrace(void)
{
  uint64 fp, fp0;
  extern char etext[];

  asm volatile("mv %0, s0" : "=r"(fp));
  fp0 = fp;
  printk("backtrace:");
  for (int i = 0; i < 16; i++) {
    // a frame lives on the stack this walk started on, and stacks grow
    // down, so every frame is at or above the first one and within a few
    // pages of it. Only the low bound was checked, and a kernel stack is
    // at a high virtual address (KSTACK is just under the trampoline):
    // so a garbage chain read its way into another process's stack, or
    // into a page that is not mapped, and the fault inside panic turned
    // one panic into a screenful of them.
    if (fp < fp0 || fp >= fp0 + 16 * PGSIZE || (fp & 7) != 0)
      break;
    uint64 ra = *(uint64 *)(fp - 8);
    uint64 next = *(uint64 *)(fp - 16);
    // and a return address is somewhere in this kernel's own text
    if (ra < KERNBASE || ra >= (uint64)etext)
      break;
    printk(" %p", (void *)ra);
    if (next <= fp)
      break;
    fp = next;
  }
  printk("\n");
}

void
panic(char *s)
{
  panicking = 1;
  printk("panic: ");
  printk("%s\n", s);
  backtrace();
  panicked = 1; // freeze uart output from other CPUs
  for (;;)
    ;
}

void
printkinit(void)
{
  initlock(&pr.lock, "pr");
}
