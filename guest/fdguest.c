//
// fdguest: a bare-metal guest (linked at 0x80400000 like hello) that
// enables its FPU and exercises F and D arithmetic across an SBI-timed
// wfi (which forces a vcpu exit and re-entry), then shuts down.
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

void
hello_main(uint64 hartid, uint64 dtb)
{
  if (hartid != 0) return;
  // enable the FPU: sstatus.FS = Initial
  asm volatile("csrs sstatus, %0" : : "r"(1L << 13));
  int fails = 0;

  // D: Basel series, 1/k^2 -> pi^2/6 ~ 1.6449
  double s = 0;
  for (int k = 1; k <= 20000; k++)
    s += 1.0 / ((double)k * k);
  puts("fdguest: basel*10000 = "); putnum((long)(s * 10000)); putc('\n');
  if ((long)(s * 10000) != 16448) fails++;

  // keep values live in FP registers across an exit (timer + wfi)
  volatile double keep = 12345.678;
  register double r1 asm("fs0") = keep;
  register float r2 asm("fs1") = 2.5f;
  sleep_ticks(500000);
  asm volatile("" : "+f"(r1), "+f"(r2));
  puts("fdguest: after wfi fs0*1000 = "); putnum((long)(r1 * 1000)); putc('\n');
  if ((long)(r1 * 1000) != 12345678 || (int)(r2 * 4) != 10) fails++;

  // F: single-precision accumulation and sqrt
  float f = 0;
  for (int i = 0; i < 100; i++) f += 0.25f;
  double q = __builtin_sqrt(2.0);
  puts("fdguest: f = "); putnum((long)f); puts(", sqrt2*1e6 = "); putnum((long)(q * 1e6)); putc('\n');
  if ((int)f != 25 || (long)(q * 1e6) != 1414213) fails++;

  // fcsr: rounding mode survives the exit as well
  asm volatile("fsrm x0, %0" : : "r"(2L));
  sleep_ticks(300000);
  long frm; asm volatile("frrm %0" : "=r"(frm));
  if (frm != 2) fails++;

  puts(fails ? "fdguest: FAIL\n" : "fdguest: ok\n");
  sbi(0x53525354, 0, 0, 0);
  for (;;) ;
}
