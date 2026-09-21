#ifndef XV6_PBKDF2_H
#define XV6_PBKDF2_H
//
// pbkdf2 over hmac-sha-256, and the string form a stored password
// takes. this is a header of static functions rather than a file of
// its own because two programs need it: the C library, so that a
// process can check a password, and the host tool that writes
// /etc/shadow when the image is built. they must agree exactly, and
// the way to be sure of that is for there to be one copy.
//
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---- sha-256 ----

struct sha256 {
  unsigned h[8];
  unsigned char buf[64];
  unsigned long long len;
  int n;
};

static const unsigned K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha256_block(struct sha256 *s, const unsigned char *p)
{
  unsigned w[64], a, b, c, d, e, f, g, h;

  for (int i = 0; i < 16; i++)
    w[i] = ((unsigned)p[4 * i] << 24) | ((unsigned)p[4 * i + 1] << 16) |
           ((unsigned)p[4 * i + 2] << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    unsigned s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    unsigned s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
  e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (int i = 0; i < 64; i++) {
    unsigned S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
    unsigned ch = (e & f) ^ (~e & g);
    unsigned t1 = h + S1 + ch + K[i] + w[i];
    unsigned S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
    unsigned mj = (a & b) ^ (a & c) ^ (b & c);
    unsigned t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
  s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void
sha256_init(struct sha256 *s)
{
  static const unsigned iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memmove(s->h, iv, sizeof(iv));
  s->len = 0;
  s->n = 0;
}

static void
sha256_update(struct sha256 *s, const void *data, size_t n)
{
  const unsigned char *p = data;

  s->len += n;
  while (n > 0) {
    size_t take = 64 - s->n;
    if (take > n)
      take = n;
    memmove(s->buf + s->n, p, take);
    s->n += take;
    p += take;
    n -= take;
    if (s->n == 64) {
      sha256_block(s, s->buf);
      s->n = 0;
    }
  }
}

static void
sha256_final(struct sha256 *s, unsigned char out[32])
{
  unsigned long long bits = s->len * 8;
  unsigned char pad = 0x80;

  sha256_update(s, &pad, 1);
  pad = 0;
  while (s->n != 56)
    sha256_update(s, &pad, 1);
  unsigned char lenb[8];
  for (int i = 0; i < 8; i++)
    lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
  sha256_update(s, lenb, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (unsigned char)(s->h[i] >> 24);
    out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
    out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
    out[4 * i + 3] = (unsigned char)s->h[i];
  }
}

// ---- hmac and pbkdf2 ----

static void
hmac_sha256(const unsigned char *key, size_t keylen, const unsigned char *msg, size_t msglen,
            const unsigned char *extra, size_t extralen, unsigned char out[32])
{
  unsigned char k[64], ipad[64], opad[64], inner[32];
  struct sha256 s;

  memset(k, 0, sizeof(k));
  if (keylen > 64) {
    sha256_init(&s);
    sha256_update(&s, key, keylen);
    sha256_final(&s, k);
  } else {
    memmove(k, key, keylen);
  }
  for (int i = 0; i < 64; i++) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  sha256_init(&s);
  sha256_update(&s, ipad, 64);
  sha256_update(&s, msg, msglen);
  if (extra)
    sha256_update(&s, extra, extralen);
  sha256_final(&s, inner);

  sha256_init(&s);
  sha256_update(&s, opad, 64);
  sha256_update(&s, inner, 32);
  sha256_final(&s, out);
}

// one 32-byte block of pbkdf2, which is all this needs
static void
pbkdf2_sha256(const char *pw, const unsigned char *salt, size_t saltlen, int rounds,
              unsigned char out[32])
{
  unsigned char u[32], t[32];
  unsigned char idx[4] = {0, 0, 0, 1};

  hmac_sha256((const unsigned char *)pw, strlen(pw), salt, saltlen, idx, 4, u);
  memmove(t, u, 32);
  for (int i = 1; i < rounds; i++) {
    hmac_sha256((const unsigned char *)pw, strlen(pw), u, 32, 0, 0, u);
    for (int k = 0; k < 32; k++)
      t[k] ^= u[k];
  }
  memmove(out, t, 32);
}

// ---- base64, in the alphabet passlib uses for this form ----

static const char B64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static int
b64_encode(const unsigned char *in, int n, char *out)
{
  int k = 0;

  for (int i = 0; i < n; i += 3) {
    unsigned v = (unsigned)in[i] << 16;
    if (i + 1 < n)
      v |= (unsigned)in[i + 1] << 8;
    if (i + 2 < n)
      v |= in[i + 2];
    out[k++] = B64[(v >> 18) & 63];
    out[k++] = B64[(v >> 12) & 63];
    if (i + 1 < n)
      out[k++] = B64[(v >> 6) & 63];
    if (i + 2 < n)
      out[k++] = B64[v & 63];
  }
  out[k] = 0;
  return k;
}

static int
b64_decode(const char *in, unsigned char *out, int max)
{
  int n = 0, bits = 0;
  unsigned v = 0;

  for (const char *p = in; *p; p++) {
    const char *q = strchr(B64, *p);
    if (q == 0 || *p == 0)
      break;
    v = (v << 6) | (unsigned)(q - B64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n < max)
        out[n++] = (unsigned char)(v >> bits);
    }
  }
  return n;
}

// ---- crypt ----

#define PBKDF2_PREFIX "$pbkdf2-sha256$"

static char crypt_result[128];

char *
crypt(const char *password, const char *setting)
{
  unsigned char salt[24], hash[32];
  char saltb64[48], hashb64[48];
  int rounds = 100000, saltlen;

  if (setting == 0 || strncmp(setting, PBKDF2_PREFIX, strlen(PBKDF2_PREFIX)) != 0)
    return 0;
  const char *p = setting + strlen(PBKDF2_PREFIX);
  rounds = (int)strtol(p, (char **)&p, 10);
  if (rounds < 1 || *p != '$')
    return 0;
  p++;
  const char *saltend = strchr(p, '$');
  int n = saltend ? (int)(saltend - p) : (int)strlen(p);
  if (n <= 0 || n >= (int)sizeof(saltb64))
    return 0;
  memmove(saltb64, p, n);
  saltb64[n] = 0;
  saltlen = b64_decode(saltb64, salt, sizeof(salt));
  if (saltlen <= 0)
    return 0;

  pbkdf2_sha256(password, salt, saltlen, rounds, hash);
  b64_encode(hash, 32, hashb64);
  snprintf(crypt_result, sizeof(crypt_result), PBKDF2_PREFIX "%d$%s$%s", rounds, saltb64, hashb64);
  return crypt_result;
}


#endif
