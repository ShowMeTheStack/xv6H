//
// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), for authenticating the migration stream
// (vmstate.c). Checked at boot against RFC 4231's test cases
// (sha256_selftest); a mismatch is printed and the stream is never authenticated with it.
//

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "sha256.h"

static const uint32 K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
block(struct sha256 *c, const uint8 *p)
{
  uint32 w[64], a, b, d, e, f, g, h, cc, t1, t2;

  for (int i = 0; i < 16; i++)
    w[i] = (uint32)p[4 * i] << 24 | (uint32)p[4 * i + 1] << 16 | (uint32)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
  e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
  for (int i = 0; i < 64; i++) {
    t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
    h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void
sha256_init(struct sha256 *c)
{
  static const uint32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  memmove(c->h, iv, sizeof(iv));
  c->len = 0;
  c->n = 0;
}

void
sha256_update(struct sha256 *c, const void *data, uint64 len)
{
  const uint8 *p = data;

  c->len += len;
  while (len > 0) {
    if (c->n == 0 && len >= 64) {
      block(c, p);
      p += 64;
      len -= 64;
      continue;
    }
    uint k = 64 - c->n < len ? 64 - c->n : (uint)len;
    memmove(c->buf + c->n, p, k);
    c->n += k;
    p += k;
    len -= k;
    if (c->n == 64) {
      block(c, c->buf);
      c->n = 0;
    }
  }
}

void
sha256_final(struct sha256 *c, uint8 out[32])
{
  uint64 bits = c->len * 8;
  uint8 pad = 0x80, zero = 0;

  sha256_update(c, &pad, 1);
  while (c->n != 56)
    sha256_update(c, &zero, 1);
  uint8 lb[8];
  for (int i = 0; i < 8; i++)
    lb[i] = bits >> (56 - 8 * i);
  sha256_update(c, lb, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = c->h[i] >> 24;
    out[4 * i + 1] = c->h[i] >> 16;
    out[4 * i + 2] = c->h[i] >> 8;
    out[4 * i + 3] = c->h[i];
  }
}

// HMAC-SHA256: the key padded (or hashed, if longer than a block) and xored with ipad/opad
void
hmac_init(struct hmac *m, const uint8 *key, uint keylen)
{
  uint8 k[64];

  memset(k, 0, sizeof(k));
  if (keylen > 64) {
    struct sha256 t;
    sha256_init(&t);
    sha256_update(&t, key, keylen);
    sha256_final(&t, k);
  } else {
    memmove(k, key, keylen);
  }
  for (int i = 0; i < 64; i++) {
    m->opad[i] = k[i] ^ 0x5c;
    k[i] ^= 0x36;
  }
  sha256_init(&m->inner);
  sha256_update(&m->inner, k, 64);
}

void
hmac_update(struct hmac *m, const void *data, uint64 len)
{
  sha256_update(&m->inner, data, len);
}

void
hmac_final(struct hmac *m, uint8 out[32])
{
  uint8 ih[32];
  struct sha256 o;

  sha256_final(&m->inner, ih);
  sha256_init(&o);
  sha256_update(&o, m->opad, 64);
  sha256_update(&o, ih, 32);
  sha256_final(&o, out);
}

// constant-time compare of two tags
int
tag_equal(const uint8 *a, const uint8 *b, uint n)
{
  uint8 d = 0;

  for (uint i = 0; i < n; i++)
    d |= a[i] ^ b[i];
  return d == 0;
}

// RFC 4231 test cases 1 and 2. Returns 1 when both pass.
int
sha256_selftest(void)
{
  static const uint8 want1[32] = {
    0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
    0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7 };
  static const uint8 want2[32] = {
    0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
    0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43 };
  uint8 key1[20], out[32];
  struct hmac m;

  memset(key1, 0x0b, sizeof(key1));
  hmac_init(&m, key1, sizeof(key1));
  hmac_update(&m, "Hi There", 8);
  hmac_final(&m, out);
  if (!tag_equal(out, want1, 32))
    return 0;
  hmac_init(&m, (const uint8 *)"Jefe", 4);
  hmac_update(&m, "what do ya want for nothing?", 28);
  hmac_final(&m, out);
  return tag_equal(out, want2, 32);
}
