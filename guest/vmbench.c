//
// vmbench: what an exit to the host costs, measured from inside a guest.
//
// Every number this prints is a round trip through the hypervisor: the
// guest does something the host has to see, the host sees it and comes
// back, and the guest counts the cycles. That is the only place the cost
// can be measured honestly -- the host's own timers cannot see the entry
// and the exit around them.
//
//   mmio-read     a load from an emulated device register (the UART's
//                 line-status register, which every guest polls)
//   mmio-write    a store to one (the UART's transmit holding register
//                 with the divisor latch clear is a byte to nowhere)
//   csr-trap      a read of a CSR the host emulates (`time' is provided
//                 by the hardware; this uses one the host has to answer)
//   sbi-null      an SBI call the host answers without doing anything
//                 (the base extension's spec-version query)
//   wfi           a wait-for-interrupt with a timer already armed, which
//                 is the exit a sleeping guest takes
//
// It is a bare S-mode payload, like guest/hello.c: no file system, no
// libc, and nothing between it and the machine.
//
//   vmm start -k /vmbench -m 16 -f none
//

typedef unsigned long uint64;
typedef unsigned int uint32;

#define UART 0x10000000L

static void
putc(char c)
{
  while (!(*(volatile unsigned char *)(UART + 5) & 0x20))
    ;
  *(volatile unsigned char *)(UART + 0) = c;
}

static void
puts(const char *s)
{
  while (*s)
    putc(*s++);
}

static void
putnum(uint64 x)
{
  char b[24];
  int i = 0;

  if (x == 0)
    b[i++] = '0';
  while (x) {
    b[i++] = '0' + x % 10;
    x /= 10;
  }
  while (i > 0)
    putc(b[--i]);
}

// a number with three decimals, from a fixed-point value scaled by 1000
static void
putfix(uint64 milli)
{
  putnum(milli / 1000);
  putc('.');
  putnum((milli / 100) % 10);
  putnum((milli / 10) % 10);
  putnum(milli % 10);
}

static uint64
rdtime(void)
{
  uint64 t;
  asm volatile("csrr %0, time" : "=r"(t));
  return t;
}

static long
sbi(long ext, long fid, long a0, long a1)
{
  register long x10 asm("a0") = a0;
  register long x11 asm("a1") = a1;
  register long x16 asm("a6") = fid;
  register long x17 asm("a7") = ext;
  asm volatile("ecall" : "+r"(x10), "+r"(x11) : "r"(x16), "r"(x17) : "memory");
  return x10;
}

// the timebase, so that cycles can be printed as nanoseconds. QEMU's
// virt machine counts at 10 MHz; the device tree says so, and a guest
// that cannot read it assumes that.
#define TIMEBASE 10000000UL

static void
report(const char *what, uint64 ticks, int n)
{
  // the counter runs at TIMEBASE, so one tick is 1e9/TIMEBASE ns. What is
  // printed is nanoseconds each to three decimals, and the total, so that
  // a number that looks wrong can be checked against the other.
  uint64 total_ns = ticks * (1000000000UL / TIMEBASE);
  uint64 per_milli = total_ns * 1000 / (uint64)n;   // ns each, x1000
  puts("vmbench: ");
  puts(what);
  puts(" ");
  putfix(per_milli);
  puts(" ns each (");
  putnum(n);
  puts(" in ");
  putnum(total_ns / 1000);
  puts(" us)\n");
}

// guest/hello.S calls this on every hart it is given; only the first one
// measures, and the others wait for the shutdown.
int
hello_main(uint64 hartid, uint64 dtb)
{
  const int N = 20000;

  (void)dtb;
  if (hartid != 0) {
    for (;;)
      asm volatile("wfi");
  }

  puts("vmbench: start\n");

  // an emulated load: the host decodes the instruction, answers from its
  // model of the device, and steps the guest's pc over it
  uint64 t0 = rdtime();
  for (int i = 0; i < N; i++)
    (void)*(volatile unsigned char *)(UART + 5);
  uint64 t1 = rdtime();
  report("mmio-read ", t1 - t0, N);

  // and an emulated store
  t0 = rdtime();
  for (int i = 0; i < N; i++)
    *(volatile unsigned char *)(UART + 4) = 0;   // MCR: harmless
  t1 = rdtime();
  report("mmio-write", t1 - t0, N);

  // a CSR the host has to answer for
  t0 = rdtime();
  for (int i = 0; i < N; i++) {
    uint64 v;
    asm volatile("csrr %0, 0xc03" : "=r"(v));    // hpmcounter3
    (void)v;
  }
  t1 = rdtime();
  report("csr-trap  ", t1 - t0, N);

  // an SBI call that does nothing
  t0 = rdtime();
  for (int i = 0; i < N; i++)
    sbi(0x10, 0, 0, 0);                          // base: spec version
  t1 = rdtime();
  report("sbi-null  ", t1 - t0, N);

  // and the exit a sleeping guest takes: wfi with a timer armed for a
  // moment from now, a thousand times.
  //
  // sie.STIE is enabled and sstatus.SIE is *not*: wfi returns when the
  // interrupt becomes pending, and nothing is taken. This guest has no
  // trap vector, so an interrupt that was taken would be the end of it.
  asm volatile("csrs sie, %0" ::"r"(1UL << 5));   // STIE
  t0 = rdtime();
  for (int i = 0; i < 1000; i++) {
    sbi(0x54494D45, 0, (long)(rdtime() + TIMEBASE / 10000), 0);  // 100 us
    asm volatile("wfi");
    // and the timer is disarmed again, so that the next wfi waits for the
    // next one rather than returning at once on a bit that is still set
    sbi(0x54494D45, 0, -1L, 0);
  }
  t1 = rdtime();
  report("wfi       ", t1 - t0, 1000);

  puts("vmbench: done\n");
  sbi(0x53525354, 0, 0, 0);   // system reset: shutdown
  for (;;)
    asm volatile("wfi");
  return 0;
}
