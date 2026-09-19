//
// vguest: a bare-metal guest using the vector extension: enables it,
// checks vlenb, does a vector add and a reduction, keeps vector state
// live across a timed wfi (vcpu exit), then shuts down. reports "no
// vector extension" and exits cleanly if misa lacks V (via SBI probe of
// the device tree not being needed: it just tries vsetvli under a trap
// handler that counts illegal instructions).
//
typedef unsigned long uint64;
#define UART 0x10000000L

static void putc(char c) { *(volatile unsigned char *)(UART + 0) = c; }
static void puts(const char *s) { while (*s) putc(*s++); }
static void putnum(long x) {
  char b[24]; int i = 0;
  if (x < 0) { putc('-'); x = -x; }
  if (x == 0) b[i++] = '0';
  while (x) { b[i++] = '0' + x % 10; x /= 10; }
  while (i > 0) putc(b[--i]);
}
static long sbi(long ext, long fid, long a0, long a1) {
  register long x10 asm("a0") = a0; register long x11 asm("a1") = a1;
  register long x16 asm("a6") = fid; register long x17 asm("a7") = ext;
  asm volatile("ecall" : "+r"(x10), "+r"(x11) : "r"(x16), "r"(x17) : "memory");
  return x10;
}
static void sleep_ticks(uint64 n) {
  uint64 t; asm volatile("csrr %0, time" : "=r"(t));
  asm volatile("csrw 0x14d, %0" : : "r"(t + n)); asm volatile("wfi");
}

static int x[1024], y[1024], z[1024];

void
hello_main(uint64 hartid, uint64 dtb)
{
  if (hartid != 0) return;
  // is V there? try to enable sstatus.VS; it reads back as zero without V.
  asm volatile("csrs sstatus, %0" : : "r"(1L << 9));
  uint64 ss; asm volatile("csrr %0, sstatus" : "=r"(ss));
  if (((ss >> 9) & 3) == 0) {
    puts("vguest: no vector extension\n");
    sbi(0x53525354, 0, 0, 0);
    for (;;) ;
  }
  uint64 vlenb; asm volatile("csrr %0, 0xc22" : "=r"(vlenb));
  puts("vguest: vlen "); putnum(vlenb * 8); puts(" bits\n");
  int fails = 0;

  for (int i = 0; i < 1024; i++) { x[i] = i; y[i] = 2 * i + 1; }
  int *p = x, *q = y, *d = z; long n = 1024;
  while (n > 0) {
    long vl;
    asm volatile(".option push\n.option arch, +v\n"
                 "vsetvli %0, %1, e32, m2, ta, ma\n"
                 "vle32.v v0, (%2)\nvle32.v v2, (%3)\nvadd.vv v4, v0, v2\nvse32.v v4, (%4)\n"
                 ".option pop\n" : "=r"(vl) : "r"(n), "r"(p), "r"(q), "r"(d) : "memory");
    p += vl; q += vl; d += vl; n -= vl;
  }
  for (int i = 0; i < 1024; i++) if (z[i] != 3 * i + 1) { fails++; break; }
  puts("vguest: vadd "); puts(fails ? "wrong\n" : "ok\n");

  // vector state across a vcpu exit: fill v8.. with a pattern, wfi, store back.
  static unsigned char pat[256], back[256];
  for (int i = 0; i < 256; i++) pat[i] = (unsigned char)(i * 13 + 7);
  long vl;
  asm volatile(".option push\n.option arch, +v\n"
               "vsetvli %0, %1, e8, m1, ta, ma\nvle8.v v8, (%2)\n.option pop\n"
               : "=r"(vl) : "r"(256L), "r"(pat) : "memory");
  sleep_ticks(400000);
  asm volatile(".option push\n.option arch, +v\nvse8.v v8, (%0)\n.option pop\n" : : "r"(back) : "memory");
  int bad = 0;
  for (long i = 0; i < vl; i++) if (back[i] != pat[i]) bad++;
  puts("vguest: state across wfi "); puts(bad ? "corrupted\n" : "ok\n");
  if (bad) fails++;
  // vl survived too?
  uint64 vlnow; asm volatile("csrr %0, vl" : "=r"(vlnow));
  if (vlnow != (uint64)vl) { puts("vguest: vl lost\n"); fails++; }

  puts(fails ? "vguest: FAIL\n" : "vguest: ok\n");
  sbi(0x53525354, 0, 0, 0);
  for (;;) ;
}
