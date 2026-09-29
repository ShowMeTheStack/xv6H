//
// "aliasguest": a payload that stresses the alias window (L33) and measures
// its cost (L20). Hart 0 speaks the revision protocol and, on every revision
// of the device map, deliberately keeps the window open -- it spins two
// milliseconds between reading the new description and acknowledging it --
// while hart 1 hammers the disk's window at BOTH its old and its new address,
// checking every read against virtio's magic. Hart 1 also zeroes a page with
// cbo.zero at the block size the description gives it and checks the page,
// so a cache-block revision is stressed the same way. Every read that returns
// the wrong value is counted and the first is remembered.
//
// Linked at 0x80400000 like hello; started as `vmm start -k /aliasguest -f /disk0.img -c 2'.
//

typedef unsigned long uint64;
typedef unsigned int uint32;
typedef unsigned char uint8;

#define UART 0x10000000L
#define SBI_MACH 0x4D414348L
#define R32(a) (*(volatile uint32 *)(a))
#define VIRTIO_MAGIC 0x74726976u

static void putc(char c) { while (!(*(volatile uint8 *)(UART + 5) & 0x20)) ; *(volatile uint8 *)(UART + 0) = c; }
static void puts(const char *s) { while (*s) putc(*s++); }
static void puthex(uint64 x) { const char *d = "0123456789abcdef"; puts("0x"); int st = 0; for (int i = 60; i >= 0; i -= 4) { int v = (x >> i) & 15; if (v || st || i == 0) { putc(d[v]); st = 1; } } }
static void putnum(uint64 x) { char b[24]; int i = 0; if (x == 0) b[i++] = '0'; while (x) { b[i++] = '0' + x % 10; x /= 10; } while (i > 0) putc(b[--i]); }

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
static long
sbi_val(long ext, long fid, long a0)
{
  register long x10 asm("a0") = a0;
  register long x11 asm("a1") = 0;
  register long x16 asm("a6") = fid;
  register long x17 asm("a7") = ext;
  asm volatile("ecall" : "+r"(x10), "+r"(x11) : "r"(x16), "r"(x17) : "memory");
  return x11;
}
static uint32 be32(const uint8 *p) { return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3]; }
static uint64 be64(const uint8 *p) { return ((uint64)be32(p) << 32) | be32(p + 4); }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

struct desc { uint64 vbase; uint32 cboz; };

// the first virtio,mmio node's reg (the disk), and the cpu's cache-block size
static int
parse(uint64 dtb, struct desc *d)
{
  const uint8 *fdt = (const uint8 *)dtb;
  if (be32(fdt) != 0xd00dfeed) return -1;
  const uint8 *p = fdt + be32(fdt + 8);
  const char *strs = (const char *)fdt + be32(fdt + 12);
  int depth = 0; const char *compat = 0; int clen = 0; uint64 reg = 0; int have_reg = 0; uint32 cboz = 0;
  d->vbase = 0; d->cboz = 64;
  for (;;) {
    uint32 tok = be32(p); p += 4;
    if (tok == 1) { depth++; while (*p) p++; p++; while ((uint64)p & 3) p++; compat = 0; clen = 0; have_reg = 0; }
    else if (tok == 2) {
      if (compat && have_reg && !d->vbase)
        for (const char *c = compat; c < compat + clen; ) { if (streq(c, "virtio,mmio")) d->vbase = reg; while (*c) c++; c++; }
      compat = 0; have_reg = 0;
      if (--depth == 0) break;
    } else if (tok == 3) {
      uint32 len = be32(p), noff = be32(p + 4); p += 8;
      const char *name = strs + noff;
      if (streq(name, "compatible")) { compat = (const char *)p; clen = len; }
      else if (streq(name, "reg") && len >= 8) { reg = be64(p); have_reg = 1; }
      else if (streq(name, "riscv,cboz-block-size") && len >= 4) cboz = be32(p);
      p += len; while ((uint64)p & 3) p++;
    } else if (tok == 4) { } else break;
  }
  if (cboz) d->cboz = cboz;
  return d->vbase ? 0 : -1;
}

// shared between the harts
static volatile uint64 vbase_cur, vbase_old;
static volatile uint32 cboz_cur;
static volatile int window_open, round_busy;
static volatile uint64 reads_cur, reads_old, errors, bad_addr, bad_val, cboz_rounds, cboz_errors, revisions;
static volatile uint32 seen_gen;
static uint8 page[8192] __attribute__((aligned(4096)));

static inline uint64 rdtime(void) { uint64 t; asm volatile("csrr %0, time" : "=r"(t)); return t; }

static void
report(void)
{
  puts("aliasguest: revisions "); putnum(revisions);
  puts(" reads cur "); putnum(reads_cur); puts(" old "); putnum(reads_old);
  puts(" errors "); putnum(errors);
  if (errors) { puts(" (first at "); puthex(bad_addr); puts(" = "); puthex(bad_val); puts(")"); }
  puts(" cboz rounds "); putnum(cboz_rounds); puts(" errors "); putnum(cboz_errors);
  puts(" block "); putnum(cboz_cur); puts("\n");
}

static void
revise(void)
{
  uint32 gen = (uint32)sbi_val(SBI_MACH, 1, 0);
  if (gen == seen_gen) return;
  uint64 dtb = sbi_val(SBI_MACH, 2, 0);
  struct desc fresh;
  if (parse(dtb, &fresh) < 0) { puts("aliasguest: bad tree\n"); return; }
  // the window: the guest knows the new map and has not acknowledged it; the
  // old windows must answer for these two milliseconds while hart 1 hammers both
  if (fresh.vbase != vbase_cur) { vbase_old = vbase_cur; vbase_cur = fresh.vbase; window_open = 1; }
  cboz_cur = fresh.cboz;
  uint64 t0 = rdtime();
  while (rdtime() - t0 < 20000) ;             // 2 ms at 10 MHz
  window_open = 0;
  // a guest may not acknowledge while one of its harts is mid-way through an
  // operation keyed by the fact: hart 1's cbo.zero round strides the block it
  // read at the round's start, and the host serves the larger stride only
  // until the ack (alias1: 2 half-zeroed pages in 1 148); a read of the old
  // window issued just before this flag went down is served only until the
  // ack too (alias2: 2 reads of zero). Linux's clear_page reads the block
  // size once per page the same way (machrev: stop_machine). Under RVWMO the
  // store above and the load below may be reordered against hart 1's (Dekker:
  // alias2's 8 half-zeroed pages with the flag but no fence): fence.
  asm volatile("fence rw,rw" ::: "memory");
  while (round_busy) ;
  seen_gen = gen;
  revisions++;
  sbi(SBI_MACH, 3, 1, 0);                      // ack REV_DEVICES
  if (revisions % 50 == 0) report();
}

void
alias_trap(void)
{
  uint64 cause; asm volatile("csrr %0, scause" : "=r"(cause));
  if (cause == 0x8000000000000001UL) { asm volatile("csrc sip, %0" : : "r"(2L)); revise(); return; }
  if (cause == 0x8000000000000009UL) { report(); (void)*(volatile uint8 *)(UART + 0); return; }   // a keypress: report
  puts("aliasguest: trap cause "); puthex(cause); puts("\n");
  uint64 epc; asm volatile("csrr %0, sepc" : "=r"(epc)); epc += 4; asm volatile("csrw sepc, %0" : : "r"(epc));
}

static void
hammer(void)
{
  uint64 n = 0;
  for (;;) {
    uint32 v = R32(vbase_cur);
    if (v != VIRTIO_MAGIC) { if (!errors) { bad_addr = vbase_cur; bad_val = v; } errors++; }
    reads_cur++;
    round_busy = 1;
    asm volatile("fence rw,rw" ::: "memory");
    if (window_open) {
      uint32 w = R32(vbase_old);
      if (w != VIRTIO_MAGIC) { if (!errors) { bad_addr = vbase_old; bad_val = w; } errors++; }
      reads_old++;
    }
    if ((++n & 63) != 0) {
      asm volatile("fence rw,rw" ::: "memory");
      round_busy = 0;
    } else {
      for (int i = 0; i < 4096; i++) page[i] = 0xff;
      uint32 step = cboz_cur ? cboz_cur : 64;
      for (uint64 off = 0; off < 4096; off += step)
        asm volatile(".insn i 0x0f, 0x2, x0, %0, 4" : : "r"(page + off) : "memory");   // cbo.zero
      int bad = 0;
      for (int i = 0; i < 4096; i++) if (page[i]) { bad = 1; break; }
      if (bad) cboz_errors++; else cboz_rounds++;
      asm volatile("fence rw,rw" ::: "memory");
      round_busy = 0;
    }
  }
}

void
alias_main(uint64 hartid, uint64 dtb)
{
  if (hartid == 1) { while (!vbase_cur) ; hammer(); }
  if (hartid != 0) return;
  struct desc d;
  if (parse(dtb, &d) < 0) { puts("aliasguest: BAD device tree\n"); for (;;) ; }
  cboz_cur = d.cboz; vbase_cur = d.vbase;
  puts("aliasguest: disk window at "); puthex(d.vbase); puts(", cache block "); putnum(d.cboz); puts("\n");
  long ver = sbi_val(SBI_MACH, 0, 0);
  seen_gen = (uint32)sbi_val(SBI_MACH, 1, 0);
  *(volatile uint8 *)(UART + 1) = 1;
  // the UART's line through the PLIC, hart 0 context 1, so a keypress reports
  R32(0x0c000000L + 4 * 10) = 1; R32(0x0c000000L + 0x2080) = 1u << 10; R32(0x0c000000L + 0x201000) = 0;
  asm volatile("csrs sie, %0" : : "r"((1L << 9) | (1L << 1)));
  asm volatile("csrs sstatus, %0" : : "r"(1L << 1));
  puts("aliasguest: revision protocol version "); putnum(ver); puts(", ready\n");
  for (;;) asm volatile("wfi");
}
