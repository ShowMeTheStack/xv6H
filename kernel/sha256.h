// SHA-256 and HMAC-SHA256 (sha256.c)
struct sha256 {
  uint32 h[8];
  uint64 len;
  uint8 buf[64];
  uint n;
};
struct hmac {
  struct sha256 inner;
  uint8 opad[64];
};
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, uint64 len);
void sha256_final(struct sha256 *c, uint8 out[32]);
void hmac_init(struct hmac *m, const uint8 *key, uint keylen);
void hmac_update(struct hmac *m, const void *data, uint64 len);
void hmac_final(struct hmac *m, uint8 out[32]);
int tag_equal(const uint8 *a, const uint8 *b, uint n);
int sha256_selftest(void);
