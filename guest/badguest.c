//
// a deliberately hostile guest: it pokes at everything it should not,
// then asks the hypervisor (via SBI) to shut it down. the host must
// print diagnostics, keep running, and free the VM.
//
// built as a bare S-mode payload at 0x80000000 (see guest/badguest.ld);
// runs entirely without paging.
//

typedef unsigned long uint64;
typedef unsigned int uint32;

#define UART 0x10000000L
#define VIRTIO 0x10001000L
#define PLIC 0x0c000000L

static void
putc(char c)
{
  *(volatile unsigned char *)(UART + 0) = c;
}

static void
puts(const char *s)
{
  while (*s)
    putc(*s++);
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

// the trap handler below lands here on any exception; it just returns
// to the instruction after the faulting one.
void badtrap(void);

void
badmain(void)
{
  asm volatile("csrw stvec, %0" : : "r"(badtrap));

  puts("badguest: hello from a hostile guest\n");

  // 1. touch unmapped MMIO: no device lives here.
  puts("badguest: unmapped mmio\n");
  *(volatile uint32 *)0x20000000L = 1;
  (void)*(volatile uint32 *)0x30000000L;

  // 2. a load from beyond the end of RAM (guest-physical, no mapping).
  puts("badguest: beyond ram\n");
  (void)*(volatile uint32 *)0x90000000L;

  // 3. garbage virtio configuration: queue at an absurd address, then notify.
  puts("badguest: bad virtqueue\n");
  *(volatile uint32 *)(VIRTIO + 0x030) = 0;           // queue sel
  *(volatile uint32 *)(VIRTIO + 0x038) = 8;           // queue num
  *(volatile uint32 *)(VIRTIO + 0x080) = 0xdeadbeef;  // desc low: not RAM
  *(volatile uint32 *)(VIRTIO + 0x084) = 0x7;
  *(volatile uint32 *)(VIRTIO + 0x090) = 0x1000;      // avail low: below RAM
  *(volatile uint32 *)(VIRTIO + 0x094) = 0;
  *(volatile uint32 *)(VIRTIO + 0x0a0) = 0xfffff000;
  *(volatile uint32 *)(VIRTIO + 0x0a4) = 0xffffffff;
  *(volatile uint32 *)(VIRTIO + 0x044) = 1;           // ready
  *(volatile uint32 *)(VIRTIO + 0x050) = 0;           // notify
  // a queue in RAM but with a self-referencing descriptor chain.
  volatile uint64 *desc = (volatile uint64 *)0x80100000L;
  volatile unsigned short *avail = (volatile unsigned short *)0x80101000L;
  for (int i = 0; i < 8; i++) {
    desc[2 * i] = 0x80102000L;                 // addr
    desc[2 * i + 1] = (uint64)16 | ((uint64)1 << 32) | ((uint64)i << 48); // len, flags NEXT, next = self
  }
  avail[0] = 0;
  avail[1] = 1;
  avail[2] = 0;
  *(volatile uint32 *)(VIRTIO + 0x080) = 0x80100000;
  *(volatile uint32 *)(VIRTIO + 0x084) = 0;
  *(volatile uint32 *)(VIRTIO + 0x090) = 0x80101000;
  *(volatile uint32 *)(VIRTIO + 0x094) = 0;
  *(volatile uint32 *)(VIRTIO + 0x0a0) = 0x80103000;
  *(volatile uint32 *)(VIRTIO + 0x0a4) = 0;
  *(volatile uint32 *)(VIRTIO + 0x050) = 0;

  // 4. wild PLIC/APLIC/IMSIC register offsets and huge context numbers.
  puts("badguest: wild intc\n");
  *(volatile uint32 *)(PLIC + 0x5ffffc) = 0xffffffff;
  *(volatile uint32 *)(PLIC + 0x201004 + 0x1000 * 200) = 77;
  *(volatile uint32 *)(0x0d000000L + 0x7ffc) = 0xffffffff;
  *(volatile uint32 *)(0x28000000L + 0x10) = 0xffffffff;

  // 5. machine-mode CSRs and privileged instructions we cannot have.
  puts("badguest: forbidden csrs\n");
  uint64 x;
  asm volatile("csrr %0, 0xf14" : "=r"(x));       // mhartid: emulated
  asm volatile("csrw 0x305, %0" : : "r"(x));      // mtvec: emulated, ignored
  asm volatile("csrr %0, 0x3b1" : "=r"(x));       // pmpaddr1: not emulated -> illegal
  asm volatile("csrr %0, 0x600" : "=r"(x));       // hstatus: illegal in VS
  asm volatile(".word 0x00000000");               // illegal instruction
  // wfi traps to the hypervisor (VTW); arm the timer so that it returns.
  asm volatile("csrr %0, time" : "=r"(x));
  asm volatile("csrw 0x14d, %0" : : "r"(x + 200000)); // stimecmp (Sstc)
  asm volatile("wfi");

  // 6. an unsupported SBI call, then a proper shutdown.
  puts("badguest: sbi\n");
  sbi(0x12345678, 0, 0, 0);
  puts("badguest: done, shutting down\n");
  sbi(0x53525354, 0, 0, 0); // SRST
  for (;;)
    ;
}
