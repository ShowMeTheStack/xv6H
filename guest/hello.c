//
// "hello": a guest that is not xv6. it is linked at 0x80400000 (not the
// usual 0x80000000), runs on every vcpu it is given, talks to the UART
// directly and through SBI, arms the Sstc timer, sleeps in wfi, parses
// the device tree header it was handed, and finally asks for shutdown.
// built both as an ELF and as a raw binary (guest/hello.bin).
//

typedef unsigned long uint64;
typedef unsigned int uint32;

#define UART 0x10000000L

static volatile int lock;
static volatile int harts_seen;
static volatile int go;

static void
acquire(void)
{
  while (__sync_lock_test_and_set(&lock, 1))
    ;
}

static void
release(void)
{
  __sync_lock_release(&lock);
}

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
puthex(uint64 x)
{
  const char *d = "0123456789abcdef";
  puts("0x");
  for (int i = 60; i >= 0; i -= 4)
    putc(d[(x >> i) & 15]);
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

static void
sbi_puts(const char *s)
{
  while (*s)
    sbi(0x01, 0, *s++, 0);
}

static uint32
be32(const unsigned char *p)
{
  return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static void
sleep_ticks(uint64 n)
{
  uint64 t;
  asm volatile("csrr %0, time" : "=r"(t));
  asm volatile("csrw 0x14d, %0" : : "r"(t + n)); // stimecmp
  asm volatile("wfi");
}

void
hello_main(uint64 hartid, uint64 dtb)
{
  if (hartid != 0) {
    while (!go)
      ;
    acquire();
    puts("hello: hart ");
    putnum(hartid);
    puts(" alive\n");
    __sync_fetch_and_add(&harts_seen, 1);
    release();
    return; // parks in wfi (see hello.S)
  }

  uint64 pc;
  asm volatile("auipc %0, 0" : "=r"(pc));
  puts("hello: running at ");
  puthex(pc & ~0xfffL);
  puts(", hart 0, dtb at ");
  puthex(dtb);
  putc('\n');

  const unsigned char *fdt = (const unsigned char *)dtb;
  if (be32(fdt) == 0xd00dfeed) {
    puts("hello: device tree ok, ");
    putnum(be32(fdt + 4));
    puts(" bytes\n");
  } else {
    puts("hello: BAD device tree magic\n");
  }

  sbi_puts("hello: sbi console works\n");

  long v = sbi(0x10, 0, 0, 0); // base: get_spec_version
  puts("hello: sbi spec version call returned ");
  putnum(v);
  putc('\n');

  __sync_fetch_and_add(&harts_seen, 1);
  go = 1;
  sleep_ticks(2000000); // ~0.2 s: let the other harts report
  acquire();
  puts("hello: ");
  putnum(harts_seen);
  puts(" hart(s) total\n");
  puts("hello: done\n");
  release();
  sbi(0x53525354, 0, 0, 0); // shutdown
  for (;;)
    ;
}
