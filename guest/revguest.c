//
// "revguest": a second, independent guest that speaks the machine-revision
// protocol. It is not xv6 and shares no code with it -- about three hundred
// lines that parse the device tree they are handed, bind an interrupt driver to
// whichever controller the tree describes (a PLIC, or an APLIC-MSI+IMSIC), take
// the UART's interrupt through it, and, when the hypervisor says the description
// has changed, read the tree again and rebind. It exists so that "the guest
// speaks the protocol" is shown to mean the protocol, not xv6.
//
// Linked at 0x80400000 like hello; started as `vmm start -k /revguest -f none'.
//

typedef unsigned long uint64;
typedef unsigned int uint32;
typedef unsigned char uint8;

#define UART 0x10000000L
#define UART_IRQ 10

static void
putc(char c)
{
  while (!(*(volatile uint8 *)(UART + 5) & 0x20))
    ;
  *(volatile uint8 *)(UART + 0) = c;
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
  int started = 0;
  for (int i = 60; i >= 0; i -= 4) {
    int v = (x >> i) & 15;
    if (v || started || i == 0) { putc(d[v]); started = 1; }
  }
}

static void
putnum(uint64 x)
{
  char b[24]; int i = 0;
  if (x == 0) b[i++] = '0';
  while (x) { b[i++] = '0' + x % 10; x /= 10; }
  while (i > 0) putc(b[--i]);
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

static long
sbi_val(long ext, long fid, long a0)
{
  register long x10 asm("a0") = a0;
  register long x11 asm("a1") = 0;
  register long x16 asm("a6") = fid;
  register long x17 asm("a7") = ext;
  asm volatile("ecall" : "+r"(x10), "+r"(x11) : "r"(x16), "r"(x17) : "memory");
  return x11;   // sbiret.value
}

#define SBI_MACH 0x4D414348L

// ---- a device tree, read just far enough ----

static uint32
be32(const uint8 *p)
{
  return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static uint64
be64(const uint8 *p)
{
  return ((uint64)be32(p) << 32) | be32(p + 4);
}

static int
streq(const char *a, const char *b)
{
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

// what this guest wants to know about its machine
struct desc {
  int kind;            // 0 plic, 1 aplic (direct), 2 aplic-msi + imsic
  uint64 plic, aplic_s, imsic_s;
};

// walk the structure block. A node's `compatible' names what it is and its
// `reg' says where; the S-level APLIC/IMSIC nodes are told apart from the
// M-level ones by the "riscv,ipi"-less way fdtgen names them: the S domain is
// the one whose interrupt-controller node has an "interrupts-extended" pointing
// at S-mode, but simpler and sufficient here: the higher address is the S
// domain (0x0d000000 over 0x0c000000; 0x28000000 over 0x24000000).
static int
parse(uint64 dtb, struct desc *d)
{
  const uint8 *fdt = (const uint8 *)dtb;
  if (be32(fdt) != 0xd00dfeed)
    return -1;
  const uint8 *st = fdt + be32(fdt + 8);
  const char *strs = (const char *)fdt + be32(fdt + 12);
  const uint8 *p = st;
  int depth = 0;
  const char *compat = 0; int compat_len = 0;
  uint64 reg = 0; int have_reg = 0;
  uint64 aplic_hi = 0, imsic_hi = 0;
  d->kind = 0; d->plic = d->aplic_s = d->imsic_s = 0;
  int msi = 0;
  for (;;) {
    uint32 tok = be32(p); p += 4;
    if (tok == 1) {                          // BEGIN_NODE
      depth++;
      while (*p) p++;
      p++;
      while ((uint64)p & 3) p++;
      compat = 0; compat_len = 0; have_reg = 0;
    } else if (tok == 2) {                   // END_NODE
      if (compat && have_reg) {
        for (const char *c = compat; c < compat + compat_len; ) {
          if (streq(c, "riscv,plic0")) d->plic = reg;
          else if (streq(c, "riscv,aplic")) { if (reg > aplic_hi) aplic_hi = reg; }
          else if (streq(c, "riscv,imsics")) { if (reg > imsic_hi) imsic_hi = reg; }
          while (*c) c++;
          c++;
        }
      }
      compat = 0; have_reg = 0;
      if (--depth == 0) break;
    } else if (tok == 3) {                   // PROP
      uint32 len = be32(p); uint32 noff = be32(p + 4); p += 8;
      const char *name = strs + noff;
      if (streq(name, "compatible")) { compat = (const char *)p; compat_len = len; }
      else if (streq(name, "reg") && len >= 8) { reg = be64(p); have_reg = 1; }
      else if (streq(name, "msi-parent")) msi = 1;
      p += len;
      while ((uint64)p & 3) p++;
    } else if (tok == 4) {                   // NOP
    } else {                                 // END
      break;
    }
  }
  d->aplic_s = aplic_hi;
  d->imsic_s = imsic_hi;
  if (d->imsic_s && msi) d->kind = 2;
  else if (d->aplic_s) d->kind = 1;
  else d->kind = 0;
  return 0;
}

// ---- the drivers, as small as they can be ----

#define R32(a) (*(volatile uint32 *)(a))

static void
plic_bind(uint64 base, int on)
{
  if (!on) { R32(base + 0x2080) = 0; return; }              // S-mode enable, hart 0
  R32(base + 4 * UART_IRQ) = 1;                            // priority
  R32(base + 0x2080) = 1u << UART_IRQ;                     // enable
  R32(base + 0x201000) = 0;                                // threshold
}

static int
plic_claim(uint64 base)
{
  int irq = R32(base + 0x201004);
  if (irq) R32(base + 0x201004) = irq;                     // complete
  return irq;
}

static inline void w_siselect(uint64 x) { asm volatile("csrw 0x150, %0" : : "r"(x)); }
static inline void w_sireg(uint64 x)    { asm volatile("csrw 0x151, %0" : : "r"(x)); }
static inline uint64 claim_stopei(void) { uint64 x; asm volatile("csrrw %0, 0x15c, x0" : "=r"(x)); return x; }

static void
imsic_bind(uint64 aplic_s, int on)
{
  if (!on) {
    w_siselect(0x70); w_sireg(0);                          // eidelivery off
    return;
  }
  // the S-domain APLIC routes the UART's line to hart 0, guest file 0, identity 10
  R32(aplic_s + 0x0000) = 0;                               // domaincfg: off while configuring
  R32(aplic_s + 0x0004 + (UART_IRQ - 1) * 4) = 6;          // sourcecfg: level, active high
  R32(aplic_s + 0x3004 + (UART_IRQ - 1) * 4) = UART_IRQ;   // target: hart 0, eiid 10
  R32(aplic_s + 0x1edc) = UART_IRQ;                        // setienum
  R32(aplic_s + 0x0000) = (1 << 8) | (1 << 2);             // IE | DM (MSI)
  // and this hart's interrupt file
  w_siselect(0x70); w_sireg(0);                            // eidelivery
  w_siselect(0x72); w_sireg(0);                            // eithreshold
  w_siselect(0xC0 + 2 * (UART_IRQ / 64));                  // eie0
  asm volatile("csrs 0x151, %0" : : "r"(1UL << (UART_IRQ % 64)));
  w_siselect(0x70); w_sireg(1);
}

static int
imsic_claim(uint64 aplic_s)
{
  int id = (int)(claim_stopei() >> 16);
  if (id)
    R32(aplic_s + 0x2000) = id;                            // setipnum_le: re-pend if still asserted
  return id;
}

// ---- the machine, and what to do when it changes ----

static struct desc cur;
static volatile uint64 irqs, irqs_plic, irqs_imsic;
static volatile uint32 seen_gen;
static const char *kindname[] = {"plic", "aplic", "imsic"};

static void
bind(struct desc *d)
{
  if (d->kind == 0) plic_bind(d->plic, 1);
  else imsic_bind(d->aplic_s, 1);
}

static void
unbind(struct desc *d)
{
  if (d->kind == 0) plic_bind(d->plic, 0);
  else imsic_bind(d->aplic_s, 0);
}

static void
revise(void)
{
  uint32 gen = (uint32)sbi_val(SBI_MACH, 1, 0);          // pending
  if (gen == seen_gen)
    return;
  uint64 dtb = sbi_val(SBI_MACH, 2, 0);                  // describe
  struct desc fresh;
  if (parse(dtb, &fresh) < 0) { puts("revguest: bad tree\n"); return; }
  uint32 adopted = 0;
  if (fresh.kind != cur.kind || fresh.plic != cur.plic || fresh.aplic_s != cur.aplic_s) {
    unbind(&cur);
    cur = fresh;
    bind(&cur);
    adopted |= 2;                                        // REV_INTC
  }
  seen_gen = gen;
  sbi(SBI_MACH, 3, adopted, gen);                        // ack, named (defect 20)
  puts("revguest: revision "); putnum(gen); puts(": now on "); puts(kindname[cur.kind]);
  puts(" (adopted "); puthex(adopted); puts(")\n");
}

void
rev_trap(void)
{
  uint64 cause;
  asm volatile("csrr %0, scause" : "=r"(cause));
  if (cause == 0x8000000000000009UL) {                   // external
    int irq = cur.kind == 0 ? plic_claim(cur.plic) : imsic_claim(cur.aplic_s);
    if (irq == UART_IRQ) {
      int c = *(volatile uint8 *)(UART + 0);            // drain the byte
      irqs++;
      if (cur.kind == 0) irqs_plic++; else irqs_imsic++;
      puts("revguest: irq via "); puts(kindname[cur.kind]);
      puts(" (key "); putc((char)c); puts(", total "); putnum(irqs); puts(")\n");
    }
  } else if (cause == 0x8000000000000001UL) {            // software: the host says the machine changed
    asm volatile("csrc sip, %0" : : "r"(2L));
    revise();
  } else {
    puts("revguest: trap cause "); puthex(cause); puts("\n");
    uint64 epc; asm volatile("csrr %0, sepc" : "=r"(epc)); epc += 4;
    asm volatile("csrw sepc, %0" : : "r"(epc));
  }
}

void
rev_main(uint64 hartid, uint64 dtb)
{
  if (hartid != 0)
    return;
  puts("revguest: hart 0, dtb at "); puthex(dtb); puts("\n");
  if (parse(dtb, &cur) < 0) { puts("revguest: BAD device tree\n"); for (;;) ; }
  puts("revguest: interrupt controller "); puts(kindname[cur.kind]);
  puts(" at "); puthex(cur.kind == 0 ? cur.plic : cur.aplic_s); puts("\n");
  long ver = sbi_val(SBI_MACH, 0, 0);                    // announce: we speak revision
  puts("revguest: revision protocol version "); putnum(ver); puts("\n");
  seen_gen = (uint32)sbi_val(SBI_MACH, 1, 0);
  *(volatile uint8 *)(UART + 1) = 1;                     // UART: receive interrupt on
  bind(&cur);
  asm volatile("csrs sie, %0" : : "r"((1L << 9) | (1L << 1)));   // SEIE | SSIE
  asm volatile("csrs sstatus, %0" : : "r"(1L << 1));             // SIE
  puts("revguest: ready\n");
  for (;;)
    asm volatile("wfi");
}
