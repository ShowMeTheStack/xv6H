//
// fuzzguest: a guest that spends its life doing things a guest should
// not, at random, and then asks to be shut down.
//
// What it drives, from inside the machine, is the host's emulation:
// the virtio device models, the interrupt controllers, the CSR
// emulator, the instruction decoder, and the G-stage fault path. The
// host is what is under test -- the guest is expected to take
// hundreds of traps and to end up with its own state in ruins.
//
// the seed and the number of rounds come from the kernel command line
// (/chosen/bootargs in the device tree): "seed=N rounds=M". The seed
// is printed, so a run that finds something can be run again.
//
// built as a bare S-mode payload at 0x80000000, no paging.
//

typedef unsigned long uint64;
typedef unsigned int uint32;
typedef unsigned short uint16;

#define UART   0x10000000L
#define VIRTIO 0x10001000L   // block
#define VNET   0x10002000L
#define VBALL  0x10003000L
#define PLIC   0x0c000000L
#define APLIC_S 0x0d000000L
#define IMSIC_S 0x28000000L
#define CLINT  0x02000000L
#define RTC    0x00101000L
#define RAM    0x80000000L

uint64 fuzz_traps;           // the trap handler counts them
void fuzz_trap(void);        // in fuzzentry.S

static void
putc(char c)
{
  *(volatile unsigned char *)(UART + 0) = c;
}

// the rounds below write nonsense to the UART's own registers, and one
// of those writes sets the divisor-latch bit -- after which a byte
// written to offset 0 sets the baud rate instead of appearing on the
// console. Every message therefore puts the line back into 8N1 first,
// so that a guest which has been fuzzing its own console can still
// report what it found.
static void
puts(const char *s)
{
  *(volatile unsigned char *)(UART + 3) = 0x03; // LCR: 8 bits, no DLAB
  *(volatile unsigned char *)(UART + 1) = 0x00; // IER: no interrupts
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

// ---- the device tree: enough of it to find the command line ----

static uint32
be32(const unsigned char *p)
{
  return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static int
streq(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

// walk the structure block for /chosen/bootargs. The flattened tree is
// a stream of tokens: 1 opens a node (name follows, padded to 4), 3 is
// a property (length, name offset, value), 2 closes, 4 is a no-op and
// 9 ends. Nothing here allocates or recurses.
static const char *
bootargs(uint64 dtb)
{
  const unsigned char *fdt = (const unsigned char *)dtb;
  if (fdt == 0 || be32(fdt) != 0xd00dfeed)
    return 0;
  const unsigned char *p = fdt + be32(fdt + 8);      // off_dt_struct
  const char *strings = (const char *)(fdt + be32(fdt + 12));
  int depth = 0, in_chosen = 0;
  for (;;) {
    uint32 tok = be32(p);
    p += 4;
    if (tok == 9)                                    // FDT_END
      return 0;
    if (tok == 4)                                    // FDT_NOP
      continue;
    if (tok == 1) {                                  // FDT_BEGIN_NODE
      const char *name = (const char *)p;
      while (*p)
        p++;
      p = (const unsigned char *)(((uint64)p + 4) & ~3UL);
      depth++;
      if (depth == 2 && streq(name, "chosen"))
        in_chosen = 1;
      continue;
    }
    if (tok == 2) {                                  // FDT_END_NODE
      if (in_chosen && depth == 2)
        in_chosen = 0;
      depth--;
      if (depth < 0)
        return 0;
      continue;
    }
    if (tok == 3) {                                  // FDT_PROP
      uint32 len = be32(p), nameoff = be32(p + 4);
      const char *pname = strings + nameoff;
      const char *val = (const char *)(p + 8);
      p += 8 + ((len + 3) & ~3U);
      if (in_chosen && streq(pname, "bootargs") && len > 0)
        return val;
      continue;
    }
    return 0;                                        // a token we do not know
  }
}

// "seed=1234" out of the command line; 0 if it is not there
static uint64
argnum(const char *args, const char *key)
{
  if (args == 0)
    return 0;
  for (const char *p = args; *p; p++) {
    if (p != args && p[-1] != ' ')
      continue;
    const char *k = key, *q = p;
    while (*k && *k == *q)
      k++, q++;
    if (*k || *q != '=')
      continue;
    q++;
    uint64 v = 0;
    int any = 0;
    while (*q >= '0' && *q <= '9') {
      v = v * 10 + (uint64)(*q++ - '0');
      any = 1;
    }
    if (any)
      return v;
  }
  return 0;
}

// ---- the random source: xorshift, so that a seed is a whole run ----

static uint64 rng;

static uint64
next(void)
{
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

// ---- generated instructions ----
//
// a CSR number is part of the instruction, so an instruction that
// names a random CSR has to be written at run time. The buffer holds
// the instruction and a `ret'; the payload calls it.
static uint32 codebuf[4] __attribute__((aligned(16)));

static void
run_insn(uint32 insn)
{
  codebuf[0] = insn;
  codebuf[1] = 0x00008067; // ret
  asm volatile("fence.i" ::: "memory");
  ((void (*)(void))codebuf)();
}

// csrrs t0, <csr>, x0 -- a read of an arbitrary CSR, and with funct3
// picked at random the write forms too.
//
// the register forms always name x0, and the immediate forms carry a
// value below 32. Neither can put a CSR into a state the payload
// cannot come back from: a wide random value written to satp would
// turn on paging with a page table made of nothing, and the next
// instruction fetched would be the last. What is being driven here is
// the host's CSR emulation -- which CSR numbers it knows, what it does
// with the ones it does not -- and that is driven just as well by a
// zero as by a random word.
static uint32
csr_insn(uint32 csr, uint32 funct3, uint32 rs1)
{
  return ((csr & 0xfff) << 20) | ((rs1 & 0x1f) << 15) | ((funct3 & 7) << 12) |
         (5 << 7) | 0x73;   // rd = t0 (x5), opcode SYSTEM
}

// the opcodes this machine does not implement: custom-0 to custom-3,
// one reserved, and the 48-bit escape. Every one of them raises an
// illegal-instruction exception, which is the path being driven.
static uint32
illegal_opcode(uint64 r)
{
  static const uint32 ops[] = {0x0b, 0x2b, 0x5b, 0x6b, 0x7b, 0x7f};
  return ops[r % (sizeof(ops) / sizeof(ops[0]))];
}

// the system instructions that are not CSR accesses: each one is a
// path of its own through the hypervisor.
static void
system_insn(uint64 r)
{
  switch (r % 6) {
  case 0: {
    // a wfi with nothing to wake it would idle for ever, and this
    // guest has a job to finish: the timer is armed first, which is
    // also the path the host's timer emulation is meant to take.
    uint64 t;
    asm volatile("csrr %0, time" : "=r"(t));
    asm volatile("csrw 0x14d, %0" : : "r"(t + 100000)); // stimecmp
    asm volatile("wfi");
    break;
  }
  case 1:
    asm volatile("sfence.vma");
    break;
  case 2:
    asm volatile("sfence.vma %0, %1" : : "r"(next()), "r"(next() & 0xffff));
    break;
  case 3:
    asm volatile(".word 0x22000073"); // hfence.vvma: not ours to run
    break;
  case 4:
    asm volatile(".word 0x62000073"); // hfence.gvma: nor this
    break;
  case 5:
    asm volatile("ebreak");
    break;
  }
}

static void
mmio_round(uint64 r)
{
  static const uint64 bases[] = {VIRTIO, VNET, VBALL, PLIC, APLIC_S,
                                 IMSIC_S, CLINT, RTC, UART};
  uint64 base = bases[(r >> 3) % (sizeof(bases) / sizeof(bases[0]))];
  uint64 off = (next() % 0x2000) & ~3UL;
  volatile uint32 *p = (volatile uint32 *)(base + off);
  if (r & 1)
    *p = (uint32)next();
  else
    (void)*p;
}

// a virtqueue whose descriptors point anywhere at all, and a doorbell
// to make the host walk it
static void
queue_round(uint64 r)
{
  static const uint64 devs[] = {VIRTIO, VNET, VBALL};
  uint64 dev = devs[(r >> 5) % 3];
  volatile uint64 *desc = (volatile uint64 *)(RAM + 0x00200000L);
  volatile uint16 *avail = (volatile uint16 *)(RAM + 0x00201000L);
  for (int i = 0; i < 8; i++) {
    desc[2 * i] = next() & 0xffffffffffffULL;        // addr: anywhere
    desc[2 * i + 1] = (next() & 0xffff) | ((uint64)(next() & 3) << 32) |
                      ((uint64)(next() & 7) << 48);  // len, flags, next
  }
  avail[0] = (uint16)next();
  avail[1] = (uint16)next();
  for (int i = 0; i < 8; i++)
    avail[2 + i] = (uint16)next();
  *(volatile uint32 *)(dev + 0x030) = (uint32)(next() & 3);   // queue sel
  *(volatile uint32 *)(dev + 0x038) = (uint32)(next() & 0xff); // queue num
  *(volatile uint32 *)(dev + 0x080) = (uint32)(RAM + 0x00200000L);
  *(volatile uint32 *)(dev + 0x084) = 0;
  *(volatile uint32 *)(dev + 0x090) = (uint32)(RAM + 0x00201000L);
  *(volatile uint32 *)(dev + 0x094) = 0;
  *(volatile uint32 *)(dev + 0x0a0) = (uint32)(RAM + 0x00202000L);
  *(volatile uint32 *)(dev + 0x0a4) = 0;
  *(volatile uint32 *)(dev + 0x044) = 1;                       // ready
  *(volatile uint32 *)(dev + 0x050) = (uint32)(next() & 3);    // notify
}

// somewhere in the guest-physical space that is probably not there
static void
gstage_round(uint64 r)
{
  // anywhere but the guest's own RAM: a write into that would be this
  // guest destroying its own stack, which tests nothing and ends the
  // run early. Above 4 GB is beyond any memory a VM is given here;
  // between 0x20000000 and RAM there is nothing but a device or two.
  uint64 addr = (r & 4) ? 0x100000000UL + (next() % (1UL << 35))
                        : 0x20000000UL + (next() % 0x50000000UL);
  addr &= ~7UL;
  if (r & 2)
    *(volatile uint64 *)addr = next();
  else
    (void)*(volatile uint64 *)addr;
}

static void
sbi_round(uint64 r)
{
  long ext = (long)(next() & 0xffffffff);
  long fid = (long)(next() & 0xff);
  if ((r & 7) == 0)
    ext = 0x10;             // the base extension, with a random function
  else if ((r & 7) == 1) {  // the revision protocol (L21): every fid, random masks,
    ext = 0x4D414348;       // requests by the thousand, acks for nothing
    fid = (long)(next() & 7);
  }
  sbi(ext, fid, (long)next(), (long)next());
}

// harts other than 0 fuzz too, with a stream of their own, and say
// nothing: what they are for is two vcpus inside one emulated device
// at the same time.
static volatile int stop_others;
// and they wait for hart 0 to have said what seed it is using: a host
// message caused by one hart can land in the middle of the line the
// other is printing, and the first line of this guest's output is the
// one a test is waiting for.
static volatile int start_others;

void
fuzz_main(uint64 hartid, uint64 dtb)
{
  const char *args = bootargs(dtb);
  uint64 seed = argnum(args, "seed");
  uint64 rounds = argnum(args, "rounds");

  if (seed == 0) {
    uint64 t, c;
    asm volatile("csrr %0, time" : "=r"(t));
    asm volatile("rdcycle %0" : "=r"(c));
    seed = (t << 16) ^ c ^ 0x9e3779b97f4a7c15ULL;
  }
  if (rounds == 0)
    rounds = 2000;
  rng = seed;

  if (hartid != 0) {
    // a second stream, from a second vcpu, for as long as hart 0 runs
    while (!start_others)
      ;
    rng = seed + hartid * 0x9e3779b9UL;
    while (!stop_others) {
      uint64 r = next();
      if (r % 3 == 0)
        mmio_round(r);
      else if (r % 3 == 1)
        queue_round(r);
      else
        gstage_round(r);
    }
    return;
  }

  // twice, because the host's console is shared: a message the host
  // prints about this guest can land in the middle of a line the guest
  // is printing, and a test waiting for this one would not see it. A
  // line that is cut in half twice in a row would be bad luck indeed.
  for (int i = 0; i < 2; i++) {
    puts("fuzzguest: seed ");
    putnum(seed);
    puts(" rounds ");
    putnum(rounds);
    putc('\n');
  }

  start_others = 1;   // the other vcpus may begin now
  for (uint64 i = 0; i < rounds; i++) {
    // the round about to run may write to stvec -- every CSR number is
    // fair game -- and a trap taken with no vector would end the run
    // at address zero rather than in the handler. Putting it back here
    // means at most one round is lost to it.
    asm volatile("csrw stvec, %0" : : "r"((uint64)fuzz_trap));
    uint64 r = next();
    switch (r % 7) {
    case 0:
    case 1:
      mmio_round(r);
      break;
    case 2: {
      uint32 funct3 = (uint32)(1 + (next() % 7));
      // rs1 is a register number in the three register forms and a
      // five-bit immediate in the four immediate ones (funct3 >= 4)
      uint32 rs1 = funct3 >= 4 ? (uint32)(next() & 0x1f) : 0;
      run_insn(csr_insn((uint32)(next() & 0xfff), funct3, rs1));
      break;
    }
    case 3:
      // an instruction the machine has no encoding for. The opcode is
      // one of the reserved and custom ones, so that what runs is
      // certainly illegal: a truly random word would as often be a
      // jump, and a guest that jumps into its own heap stops being a
      // test of the host and becomes a test of nothing.
      run_insn(((uint32)next() & ~0x7fu) | illegal_opcode(next()));
      break;
    case 4:
      system_insn(next());
      break;
    case 5:
      gstage_round(r);
      break;
    case 6:
      if ((r >> 8) % 8 == 0)
        queue_round(r);
      else
        sbi_round(r);
      break;
    }
    if ((i + 1) % 100 == 0) {
      puts("fuzzguest: ");
      putnum(i + 1);
      puts(" rounds, ");
      putnum(fuzz_traps);
      puts(" traps\n");
    }
  }

  // the machine is still able to talk, which is the point
  stop_others = 1;
  for (int i = 0; i < 2; i++) {
    puts("fuzzguest: done, ");
    putnum(rounds);
    puts(" rounds, ");
    putnum(fuzz_traps);
    puts(" traps\n");
  }
  sbi(0x53525354, 0, 0, 0); // shutdown
  for (;;)
    ;
}
