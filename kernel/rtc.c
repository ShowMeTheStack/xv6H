//
// the real-time clock: qemu's virt machine carries a goldfish RTC (a
// 64-bit count of nanoseconds since the unix epoch, read low word first,
// which latches the high word), and every guest of ours gets a virtual
// one at the same address (vrtc.c). the monotonic clock is the RISC-V
// time counter scaled by the device tree's timebase-frequency.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "platform.h"
#include "time.h"

#define RTC_TIME_LOW  0x00
#define RTC_TIME_HIGH 0x04

static struct spinlock rtclock;

// The clock is read far more often than it moves: every inode timestamp
// (ext4_write.c) and every journal commit (jbd2.c) asks for it, and a
// 4 MB dd inside a guest asked 1028 times, which as two MMIO reads apiece
// was 2056 of the 3084 emulated accesses that workload cost -- two thirds
// of everything, for a number that could have been worked out.
//
// So it is worked out. The goldfish RTC and the RISC-V time counter are
// the same clock seen twice: both come from the host, and they advance
// together. Reading the device once and adding the monotonic counter's
// progress since gives the same answer without leaving the guest, because
// the time CSR is readable directly when hcounteren.TM is set, which it
// is.
//
// It is re-read every RESYNC_NS anyway, which bounds the one case where
// the two can disagree: a machine that was paused, saved or migrated has a
// monotonic counter that did not advance while the world's clock did.
#define RESYNC_NS 60000000000UL   // a minute

static struct {
  uint64 epoch_ns;   // what the device said
  uint64 mono_ns;    // the monotonic reading when it said it
  int valid;
  int soft;          // F17: the device is gone; the clock runs on from the last reading
} rtc_off;

void
rtcinit(void)
{
  initlock(&rtclock, "rtc");
  if (plat.rtc_base == 0) {
    printk("rtc: none; CLOCK_REALTIME unavailable\n");
    return;
  }
  uint64 ns = clock_realtime_ns();
  printk("rtc: goldfish at 0x%lx, epoch seconds %ld\n", plat.rtc_base, ns / 1000000000UL);
}

int
rtc_present(void)
{
  return plat.rtc_base != 0 || rtc_off.soft;   // a software clock is still a clock
}

// F16/F17: the RTC came or went (a revision of the devices). Gone: the wall
// clock continues from the last reading, driven by the counter -- a software
// clock, as a kernel keeps when its RTC is unreadable. Back: the device is
// read again and the clock re-anchors to it.
void
rtc_rebind(void)
{
  acquire(&rtclock);
  if (plat.rtc_base == 0) {
    if (rtc_off.valid) {
      rtc_off.soft = 1;
      release(&rtclock);
      printk("rtc: gone; the wall clock continues from %ld in software\n",
             rtc_off.epoch_ns / 1000000000UL);
      return;
    }
    release(&rtclock);
    printk("rtc: gone; CLOCK_REALTIME unavailable\n");
    return;
  }
  rtc_off.soft = 0;
  rtc_off.valid = 0;
  release(&rtclock);
  uint64 ns = clock_realtime_ns();
  printk("rtc: goldfish at 0x%lx, epoch seconds %ld\n", plat.rtc_base, ns / 1000000000UL);
}

// nanoseconds since the epoch, or 0 without an RTC.
uint64
clock_realtime_ns(void)
{
  if (plat.rtc_base == 0 && !rtc_off.soft)
    return 0;

  uint64 mono = clock_monotonic_ns();
  acquire(&rtclock);
  if (rtc_off.valid && mono >= rtc_off.mono_ns &&
      (rtc_off.soft || mono - rtc_off.mono_ns < RESYNC_NS)) {
    uint64 ns = rtc_off.epoch_ns + (mono - rtc_off.mono_ns);
    release(&rtclock);
    return ns;
  }
  // the two reads must not be interleaved with another hart's: the low
  // word latches the high one.
  uint32 lo = *(volatile uint32 *)(plat.rtc_base + RTC_TIME_LOW);
  uint32 hi = *(volatile uint32 *)(plat.rtc_base + RTC_TIME_HIGH);
  uint64 ns = ((uint64)hi << 32) | lo;
  rtc_off.epoch_ns = ns;
  rtc_off.mono_ns = mono;
  rtc_off.valid = 1;
  release(&rtclock);
  return ns;
}

// nanoseconds since boot.
uint64
clock_monotonic_ns(void)
{
  // from the last rate change (F6): the counter's rate may have changed since
  // boot, and what was counted at the old rate stays counted in ns
  uint64 t = r_time() - plat.tb_t0, tb = plat.timebase ? plat.timebase : 10000000;
  return plat.tb_ns0 + (t / tb) * 1000000000UL + (t % tb) * 1000000000UL / tb;
}

// F6: the rate is about to change; what has elapsed stays as it was
void
clock_reanchor(void)
{
  plat.tb_ns0 = clock_monotonic_ns();
  plat.tb_t0 = r_time();
}

// ---- the system's random bytes ----
//
// ChaCha20 over a key stirred from the cycle counter, the real-time
// clock and interrupt timing. The virtio-rng device, when qemu offers
// one, mixes in the workstation's own generator at boot.
//
static struct spinlock randlock;
static uint32 rand_state[16];
static int rand_ready;

static inline uint32
rotl32(uint32 x, int n)
{
  return (x << n) | (x >> (32 - n));
}

static void
chacha_block(uint32 out[16], uint32 in[16])
{
  for (int i = 0; i < 16; i++)
    out[i] = in[i];
  for (int r = 0; r < 10; r++) {
    // column rounds
    for (int i = 0; i < 4; i++) {
      uint32 *a = &out[i], *b = &out[4 + i], *c = &out[8 + i], *d = &out[12 + i];
      *a += *b; *d ^= *a; *d = rotl32(*d, 16);
      *c += *d; *b ^= *c; *b = rotl32(*b, 12);
      *a += *b; *d ^= *a; *d = rotl32(*d, 8);
      *c += *d; *b ^= *c; *b = rotl32(*b, 7);
    }
    // diagonal rounds
    for (int i = 0; i < 4; i++) {
      uint32 *a = &out[i], *b = &out[4 + (i + 1) % 4], *c = &out[8 + (i + 2) % 4], *d = &out[12 + (i + 3) % 4];
      *a += *b; *d ^= *a; *d = rotl32(*d, 16);
      *c += *d; *b ^= *c; *b = rotl32(*b, 12);
      *a += *b; *d ^= *a; *d = rotl32(*d, 8);
      *c += *d; *b ^= *c; *b = rotl32(*b, 7);
    }
  }
  for (int i = 0; i < 16; i++)
    out[i] += in[i];
}

// stir in whatever entropy the caller has
void
random_add(const void *data, int len)
{
  const uint8 *p = data;
  acquire(&randlock);
  for (int i = 0; i < len; i++)
    ((uint8 *)rand_state)[i % 64] ^= p[i];
  rand_state[12]++;
  release(&randlock);
}

void
randominit(void)
{
  initlock(&randlock, "random");
  const char sigma[] = "expand 32-byte k";
  memmove(rand_state, sigma, 16);
  uint64 seed[4];
  seed[0] = r_time();
  seed[1] = r_cycle();
  seed[2] = clock_realtime_ns();
  seed[3] = (uint64)&seed;
  memmove(&rand_state[4], seed, 32);
  rand_ready = 1;
}

void
random_bytes(void *dst, int len)
{
  uint8 *out = dst;
  uint32 block[16];
  acquire(&randlock);
  if (!rand_ready) {
    release(&randlock);
    randominit();
    acquire(&randlock);
  }
  // every call stirs the cycle counter in: an attacker who sees one
  // output still cannot predict the next.
  rand_state[13] ^= (uint32)r_cycle();
  rand_state[14] ^= (uint32)(r_time() >> 8);
  while (len > 0) {
    rand_state[12]++;
    chacha_block(block, rand_state);
    int n = len < 64 ? len : 64;
    memmove(out, block, n);
    out += n;
    len -= n;
  }
  release(&randlock);
}
