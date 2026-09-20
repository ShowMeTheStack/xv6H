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
  return plat.rtc_base != 0;
}

// nanoseconds since the epoch, or 0 without an RTC.
uint64
clock_realtime_ns(void)
{
  if (plat.rtc_base == 0)
    return 0;
  // the two reads must not be interleaved with another hart's.
  acquire(&rtclock);
  uint32 lo = *(volatile uint32 *)(plat.rtc_base + RTC_TIME_LOW);
  uint32 hi = *(volatile uint32 *)(plat.rtc_base + RTC_TIME_HIGH);
  release(&rtclock);
  return ((uint64)hi << 32) | lo;
}

// nanoseconds since boot.
uint64
clock_monotonic_ns(void)
{
  uint64 t = r_time(), tb = plat.timebase ? plat.timebase : 10000000;
  return (t / tb) * 1000000000UL + (t % tb) * 1000000000UL / tb;
}

// ---- the system's random bytes ----
//
// ChaCha20 over a key stirred from the cycle counter, the real-time
// clock and interrupt timing. The virtio-rng device, when qemu offers
// one, mixes in the laptop's own generator at boot.
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
