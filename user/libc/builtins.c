//
// the handful of routines the compiler calls for operations this
// processor has no instruction for. libgcc has them all, but its
// bare-metal build is not position independent, so the ones that need
// a table of their own cannot go into a shared library. these are
// written here instead, and being ours they are linked before libgcc
// is ever consulted.
//
#include <stdint.h>

int
__clzsi2(uint32_t v)
{
  int n = 0;

  if (v == 0)
    return 32;
  if (!(v & 0xffff0000u)) { n += 16; v <<= 16; }
  if (!(v & 0xff000000u)) { n += 8;  v <<= 8;  }
  if (!(v & 0xf0000000u)) { n += 4;  v <<= 4;  }
  if (!(v & 0xc0000000u)) { n += 2;  v <<= 2;  }
  if (!(v & 0x80000000u)) { n += 1; }
  return n;
}

int
__clzdi2(uint64_t v)
{
  if (v >> 32)
    return __clzsi2((uint32_t)(v >> 32));
  return 32 + __clzsi2((uint32_t)v);
}

int
__ctzsi2(uint32_t v)
{
  int n = 0;

  if (v == 0)
    return 32;
  if (!(v & 0x0000ffffu)) { n += 16; v >>= 16; }
  if (!(v & 0x000000ffu)) { n += 8;  v >>= 8;  }
  if (!(v & 0x0000000fu)) { n += 4;  v >>= 4;  }
  if (!(v & 0x00000003u)) { n += 2;  v >>= 2;  }
  if (!(v & 0x00000001u)) { n += 1; }
  return n;
}

int
__ctzdi2(uint64_t v)
{
  if ((uint32_t)v)
    return __ctzsi2((uint32_t)v);
  return 32 + __ctzsi2((uint32_t)(v >> 32));
}

int
__ffssi2(uint32_t v)
{
  return v ? __ctzsi2(v) + 1 : 0;
}

int
__ffsdi2(uint64_t v)
{
  return v ? __ctzdi2(v) + 1 : 0;
}

int
__popcountsi2(uint32_t v)
{
  v = v - ((v >> 1) & 0x55555555u);
  v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
  v = (v + (v >> 4)) & 0x0f0f0f0fu;
  return (int)((v * 0x01010101u) >> 24);
}

int
__popcountdi2(uint64_t v)
{
  return __popcountsi2((uint32_t)v) + __popcountsi2((uint32_t)(v >> 32));
}

int
__paritysi2(uint32_t v)
{
  return __popcountsi2(v) & 1;
}

int
__paritydi2(uint64_t v)
{
  return __popcountdi2(v) & 1;
}
