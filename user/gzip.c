//
// gzip, gunzip and zcat: DEFLATE, both directions.
//
//   gzip [-cdkfqv] [-1..-9] [FILE...]
//   gunzip [-ckfqv] [FILE...]      same as gzip -d
//   zcat [FILE...]                 same as gzip -dc
//
// with no file it reads standard input and writes standard output.
// with files it replaces each FILE with FILE.gz (or FILE.gz with
// FILE), unless -c says to write to standard output and keep the
// original.
//
// everything here is RFC 1951 (deflate) and RFC 1952 (the gzip
// wrapper) written out: there is no library underneath. the
// compressor finds matches with a hash chain over three-byte
// prefixes, then codes the result with a Huffman tree built for the
// block -- or stores the block uncompressed when that is smaller,
// which is what keeps already-compressed input from growing.
//
#include "user/user.h"

// ---- the tables RFC 1951 defines ----

static const int len_base[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
  59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int len_extra[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
  4, 5, 5, 5, 5, 0
};
static const int dist_base[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
  513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const int dist_extra[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
  10, 11, 11, 12, 12, 13, 13
};
// the order the code-length code's own lengths are written in
static const int clorder[19] = {
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// ---- crc32, the gzip trailer's check ----

static uint32_t crc_table[256];
static int crc_ready;

static void
crc_init(void)
{
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
  crc_ready = 1;
}

static uint32_t
crc32_update(uint32_t crc, const unsigned char *buf, int n)
{
  if (!crc_ready)
    crc_init();
  crc ^= 0xffffffffu;
  for (int i = 0; i < n; i++)
    crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
  return crc ^ 0xffffffffu;
}

// ---- bits out ----

struct bitout {
  int fd;
  unsigned char buf[16384];
  int n;
  uint32_t bits;   // bits waiting to be written, low end first
  int nbits;
  int error;
};

static void
bo_flush(struct bitout *o)
{
  int off = 0;
  while (off < o->n) {
    int w = write(o->fd, o->buf + off, o->n - off);
    if (w <= 0) {
      o->error = 1;
      break;
    }
    off += w;
  }
  o->n = 0;
}

static void
bo_byte(struct bitout *o, int c)
{
  if (o->n == sizeof(o->buf))
    bo_flush(o);
  o->buf[o->n++] = (unsigned char)c;
}

// deflate packs bits from the low end of each byte up
static void
bo_put(struct bitout *o, uint32_t value, int count)
{
  o->bits |= (value & ((1u << count) - 1)) << o->nbits;
  o->nbits += count;
  while (o->nbits >= 8) {
    bo_byte(o, o->bits & 0xff);
    o->bits >>= 8;
    o->nbits -= 8;
  }
}

// a Huffman code is written most significant bit first, so the code
// is reversed before going out low end first
static uint32_t
bitrev(uint32_t v, int n)
{
  uint32_t r = 0;

  for (int i = 0; i < n; i++) {
    r = (r << 1) | (v & 1);
    v >>= 1;
  }
  return r;
}

static void
bo_code(struct bitout *o, uint32_t code, int len)
{
  bo_put(o, bitrev(code, len), len);
}

static void
bo_align(struct bitout *o)
{
  if (o->nbits > 0)
    bo_put(o, 0, 8 - o->nbits);
}

// ---- building a Huffman code ----

// lengths for `n` symbols with the given frequencies, none longer
// than `maxlen`. when the tree comes out too deep the frequencies are
// halved and it is built again: that always converges, and the code it
// settles on is valid, which matters more here than the last few bits
// of a perfectly optimal one.
static void
huff_lengths(const uint32_t *freq_in, int n, int maxlen, unsigned char *len)
{
  static uint32_t freq[576];
  static int left[576], right[576], parent[576];
  static uint32_t nfreq[576];

  for (int i = 0; i < n; i++)
    freq[i] = freq_in[i];

  for (;;) {
    int used = 0;
    for (int i = 0; i < n; i++) {
      len[i] = 0;
      if (freq[i])
        used++;
    }
    if (used == 0) {
      // no symbol at all: the caller gives two of them a bit each
      return;
    }
    if (used == 1) {
      for (int i = 0; i < n; i++)
        if (freq[i]) {
          len[i] = 1;
          break;
        }
      return;
    }

    // the live symbols become leaves, then the two lightest nodes are
    // joined until one is left
    int nnodes = 0;
    for (int i = 0; i < n; i++)
      if (freq[i]) {
        nfreq[nnodes] = freq[i];
        left[nnodes] = -1;
        right[nnodes] = -1;
        parent[nnodes] = i; // a leaf remembers its symbol
        nnodes++;
      }
    int alive = nnodes;
    static int dead[576];
    for (int i = 0; i < nnodes; i++)
      dead[i] = 0;

    while (alive > 1) {
      int a = -1, b = -1;
      for (int i = 0; i < nnodes; i++) {
        if (dead[i])
          continue;
        if (a < 0 || nfreq[i] < nfreq[a]) {
          b = a;
          a = i;
        } else if (b < 0 || nfreq[i] < nfreq[b]) {
          b = i;
        }
      }
      dead[a] = 1;
      dead[b] = 1;
      nfreq[nnodes] = nfreq[a] + nfreq[b];
      left[nnodes] = a;
      right[nnodes] = b;
      parent[nnodes] = -1;
      dead[nnodes] = 0;
      nnodes++;
      alive--;
    }
    int root = nnodes - 1;

    // walk the tree, writing each leaf's depth into its symbol
    static int stack[1152], depth[1152];
    int sp = 0, toodeep = 0;
    stack[sp] = root;
    depth[sp] = 0;
    sp++;
    while (sp > 0) {
      sp--;
      int node = stack[sp], d = depth[sp];
      if (left[node] < 0) {
        if (d > maxlen)
          toodeep = 1;
        len[parent[node]] = (unsigned char)(d == 0 ? 1 : d);
        continue;
      }
      stack[sp] = left[node];
      depth[sp] = d + 1;
      sp++;
      stack[sp] = right[node];
      depth[sp] = d + 1;
      sp++;
    }
    if (!toodeep)
      return;
    for (int i = 0; i < n; i++)
      if (freq[i])
        freq[i] = (freq[i] + 1) / 2;
  }
}

// the canonical codes RFC 1951 derives from a set of lengths
static void
gen_codes(const unsigned char *len, int n, uint16_t *code)
{
  int count[16], next[16];

  for (int i = 0; i < 16; i++)
    count[i] = 0;
  for (int i = 0; i < n; i++)
    count[len[i]]++;
  count[0] = 0;
  int c = 0;
  for (int i = 1; i <= 15; i++) {
    c = (c + count[i - 1]) << 1;
    next[i] = c;
  }
  for (int i = 0; i < n; i++)
    code[i] = len[i] ? (uint16_t)next[len[i]]++ : 0;
}

// ---- the compressor ----

#define WINDOW  32768
#define CHUNK   32768
#define MINMATCH 3
#define MAXMATCH 258
#define HASHBITS 15
#define HASHSIZE (1 << HASHBITS)
#define POSMAX  (WINDOW + CHUNK + MAXMATCH)
#define NOPOS   (-1)

// one thing the block is made of: a literal byte, or a match
struct token {
  uint16_t len;   // 0 for a literal
  uint16_t dist;
  unsigned char lit;
};

struct deflater {
  unsigned char win[WINDOW + CHUNK + MAXMATCH];
  int start;          // where this block's data begins in win
  int end;            // where it ends
  int head[HASHSIZE];
  int prev[POSMAX];
  int64_t base;       // win[0] is this many bytes into the stream
  struct token tok[CHUNK + 16];
  int ntok;
  uint32_t lfreq[288];
  uint32_t dfreq[30];
  int level;
};

static int
hash3(const unsigned char *p)
{
  return (int)(((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (HASHSIZE - 1));
}

static void
emit_stored(struct bitout *o, const unsigned char *data, int n, int final)
{
  bo_put(o, final ? 1 : 0, 1);
  bo_put(o, 0, 2);
  bo_align(o);
  bo_byte(o, n & 0xff);
  bo_byte(o, (n >> 8) & 0xff);
  bo_byte(o, (~n) & 0xff);
  bo_byte(o, ((~n) >> 8) & 0xff);
  for (int i = 0; i < n; i++)
    bo_byte(o, data[i]);
}

// the code-length alphabet: the two trees' lengths, run-length coded
struct cltoken {
  unsigned char sym;
  unsigned char extra;
  unsigned char nextra;
};

static int
build_cl(const unsigned char *len, int n, struct cltoken *out, uint32_t *freq)
{
  int m = 0;

  for (int i = 0; i < n;) {
    int l = len[i], run = 1;
    while (i + run < n && len[i + run] == l)
      run++;
    if (l == 0) {
      while (run >= 11) {
        int take = run > 138 ? 138 : run;
        out[m].sym = 18;
        out[m].extra = (unsigned char)(take - 11);
        out[m].nextra = 7;
        m++;
        freq[18]++;
        run -= take;
        i += take;
      }
      while (run >= 3) {
        int take = run > 10 ? 10 : run;
        out[m].sym = 17;
        out[m].extra = (unsigned char)(take - 3);
        out[m].nextra = 3;
        m++;
        freq[17]++;
        run -= take;
        i += take;
      }
      while (run-- > 0) {
        out[m].sym = 0;
        out[m].nextra = 0;
        m++;
        freq[0]++;
        i++;
      }
    } else {
      out[m].sym = (unsigned char)l;
      out[m].nextra = 0;
      m++;
      freq[l]++;
      run--;
      i++;
      while (run >= 3) {
        int take = run > 6 ? 6 : run;
        out[m].sym = 16;
        out[m].extra = (unsigned char)(take - 3);
        out[m].nextra = 2;
        m++;
        freq[16]++;
        run -= take;
        i += take;
      }
      while (run-- > 0) {
        out[m].sym = (unsigned char)l;
        out[m].nextra = 0;
        m++;
        freq[l]++;
        i++;
      }
    }
  }
  return m;
}

static void
emit_block(struct deflater *d, struct bitout *o, int final)
{
  unsigned char llen[288], dlen[30], cllen[19];
  uint16_t lcode[288], dcode[30], clcode[19];
  static struct cltoken cl[288 + 30 + 16];
  uint32_t clfreq[19];
  int raw = d->end - d->start;

  d->lfreq[256]++; // the end-of-block symbol is always there

  huff_lengths(d->lfreq, 288, 15, llen);
  huff_lengths(d->dfreq, 30, 15, dlen);

  // a tree needs two codes; give the second one to a symbol that is
  // never used rather than leaving an incomplete code behind
  int nd = 0;
  for (int i = 0; i < 30; i++)
    if (dlen[i])
      nd++;
  if (nd < 2) {
    dlen[0] = 1;
    dlen[1] = 1;
  }
  int nl = 0;
  for (int i = 0; i < 288; i++)
    if (llen[i])
      nl++;
  if (nl < 2) {
    llen[256] = 1;
    llen[llen[0] ? 1 : 0] = 1;
  }

  int hlit = 286;
  while (hlit > 257 && llen[hlit - 1] == 0)
    hlit--;
  int hdist = 30;
  while (hdist > 1 && dlen[hdist - 1] == 0)
    hdist--;

  // the two sets of lengths, run-length coded together
  unsigned char both[288 + 30];
  for (int i = 0; i < hlit; i++)
    both[i] = llen[i];
  for (int i = 0; i < hdist; i++)
    both[hlit + i] = dlen[i];
  for (int i = 0; i < 19; i++)
    clfreq[i] = 0;
  int ncl = build_cl(both, hlit + hdist, cl, clfreq);
  huff_lengths(clfreq, 19, 7, cllen);
  int hclen = 19;
  while (hclen > 4 && cllen[clorder[hclen - 1]] == 0)
    hclen--;

  gen_codes(llen, 288, lcode);
  gen_codes(dlen, 30, dcode);
  gen_codes(cllen, 19, clcode);

  // would a stored block be smaller? count the bits first
  int64_t bits = 3 + 5 + 5 + 4 + hclen * 3;
  for (int i = 0; i < ncl; i++)
    bits += cllen[cl[i].sym] + cl[i].nextra;
  for (int i = 0; i < d->ntok; i++) {
    if (d->tok[i].len == 0) {
      bits += llen[d->tok[i].lit];
    } else {
      int l = d->tok[i].len, lc = 0;
      while (lc < 28 && len_base[lc + 1] <= l)
        lc++;
      int dd = d->tok[i].dist, dc = 0;
      while (dc < 29 && dist_base[dc + 1] <= dd)
        dc++;
      bits += llen[257 + lc] + len_extra[lc] + dlen[dc] + dist_extra[dc];
    }
  }
  bits += llen[256];
  if (bits >= (int64_t)raw * 8 + 40) {
    emit_stored(o, d->win + d->start, raw, final);
    return;
  }

  bo_put(o, final ? 1 : 0, 1);
  bo_put(o, 2, 2); // a tree of this block's own
  bo_put(o, hlit - 257, 5);
  bo_put(o, hdist - 1, 5);
  bo_put(o, hclen - 4, 4);
  for (int i = 0; i < hclen; i++)
    bo_put(o, cllen[clorder[i]], 3);
  for (int i = 0; i < ncl; i++) {
    bo_code(o, clcode[cl[i].sym], cllen[cl[i].sym]);
    if (cl[i].nextra)
      bo_put(o, cl[i].extra, cl[i].nextra);
  }
  for (int i = 0; i < d->ntok; i++) {
    if (d->tok[i].len == 0) {
      bo_code(o, lcode[d->tok[i].lit], llen[d->tok[i].lit]);
    } else {
      int l = d->tok[i].len, lc = 0;
      while (lc < 28 && len_base[lc + 1] <= l)
        lc++;
      bo_code(o, lcode[257 + lc], llen[257 + lc]);
      if (len_extra[lc])
        bo_put(o, l - len_base[lc], len_extra[lc]);
      int dd = d->tok[i].dist, dc = 0;
      while (dc < 29 && dist_base[dc + 1] <= dd)
        dc++;
      bo_code(o, dcode[dc], dlen[dc]);
      if (dist_extra[dc])
        bo_put(o, dd - dist_base[dc], dist_extra[dc]);
    }
  }
  bo_code(o, lcode[256], llen[256]);
}

// the longest match for `pos` reachable from the chain, or 0
static int
longest_match(struct deflater *d, int pos, int first, int max_chain, int good,
              int *out_dist)
{
  int best = 0, bestd = 0;
  int limit = d->end - pos;

  if (limit > MAXMATCH)
    limit = MAXMATCH;
  if (limit < MINMATCH)
    return 0;
  for (int cand = first; cand != NOPOS && max_chain-- > 0;
       cand = d->prev[cand]) {
    int dist = pos - cand;
    if (dist <= 0 || dist > WINDOW)
      break;
    int l = 0;
    while (l < limit && d->win[cand + l] == d->win[pos + l])
      l++;
    if (l > best) {
      best = l;
      bestd = dist;
      if (l >= limit)
        break;
      if (l >= good && max_chain > 16)
        max_chain = 16; // long enough; stop digging
    }
  }
  if (best < MINMATCH)
    return 0;
  *out_dist = bestd;
  return best;
}

// put `pos` in the chain and answer what the chain held before
static int
chain_insert(struct deflater *d, int pos)
{
  if (pos + MINMATCH > d->end)
    return NOPOS;
  int h = hash3(d->win + pos);
  int prev = d->head[h];
  d->prev[pos] = prev;
  d->head[h] = pos;
  return prev;
}

static void
emit_literal(struct deflater *d, int c)
{
  d->tok[d->ntok].len = 0;
  d->tok[d->ntok].lit = (unsigned char)c;
  d->ntok++;
  d->lfreq[c]++;
}

static void
emit_match(struct deflater *d, int len, int dist)
{
  d->tok[d->ntok].len = (uint16_t)len;
  d->tok[d->ntok].dist = (uint16_t)dist;
  d->ntok++;
  int lc = 0;
  while (lc < 28 && len_base[lc + 1] <= len)
    lc++;
  d->lfreq[257 + lc]++;
  int dc = 0;
  while (dc < 29 && dist_base[dc + 1] <= dist)
    dc++;
  d->dfreq[dc]++;
}

// turn win[start..end) into tokens, then write the block.
//
// at each position the chain is consulted for the longest match. with
// lazy matching the match found is not taken at once: the next
// position is tried too, and if it starts a longer match this byte
// goes out as a literal instead. that one extra look is most of what
// separates a fast compressor from a good one.
static void
compress_chunk(struct deflater *d, struct bitout *o, int final)
{
  int max_chain = d->level <= 3 ? 16 : (d->level <= 6 ? 128 : 1024);
  int good = d->level <= 3 ? 8 : 32;
  int lazy = d->level >= 4;

  d->ntok = 0;
  for (int i = 0; i < 288; i++)
    d->lfreq[i] = 0;
  for (int i = 0; i < 30; i++)
    d->dfreq[i] = 0;

  int pos = d->start;
  int held_len = 0, held_dist = 0, holding = 0;

  while (pos < d->end) {
    int first = chain_insert(d, pos);
    int mdist = 0;
    int mlen = first == NOPOS ? 0
                              : longest_match(d, pos, first, max_chain, good,
                                              &mdist);

    if (!lazy) {
      if (mlen) {
        emit_match(d, mlen, mdist);
        for (int k = 1; k < mlen; k++)
          chain_insert(d, pos + k);
        pos += mlen;
      } else {
        emit_literal(d, d->win[pos]);
        pos++;
      }
      continue;
    }

    if (holding) {
      if (mlen > held_len) {
        // the match one byte later is longer: the held one loses its
        // first byte, which goes out alone
        emit_literal(d, d->win[pos - 1]);
        held_len = mlen;
        held_dist = mdist;
        pos++;
      } else {
        // take the held match, which begins at pos-1
        int at = pos - 1;
        emit_match(d, held_len, held_dist);
        for (int k = 1; k < held_len; k++)
          if (at + k > pos)
            chain_insert(d, at + k); // `pos` is already in
        pos = at + held_len;
        holding = 0;
      }
      continue;
    }

    if (mlen) {
      held_len = mlen;
      held_dist = mdist;
      holding = 1;
      pos++;
    } else {
      emit_literal(d, d->win[pos]);
      pos++;
    }
  }
  if (holding) {
    // the last thing found was held and the data ran out; it still fits
    // because a match never reaches past end
    emit_match(d, held_len, held_dist);
  }
  emit_block(d, o, final);
}

static struct deflater defl;

static int
compress(int in, int out, int level)
{
  struct bitout o;
  uint32_t crc = 0;
  int64_t total = 0;

  memset(&o, 0, sizeof(o));
  o.fd = out;
  memset(&defl, 0, sizeof(defl));
  defl.level = level;
  for (int i = 0; i < HASHSIZE; i++)
    defl.head[i] = NOPOS;
  for (int i = 0; i < POSMAX; i++)
    defl.prev[i] = NOPOS;

  // the gzip header: magic, deflate, no flags, no time, this level, unix
  bo_byte(&o, 0x1f);
  bo_byte(&o, 0x8b);
  bo_byte(&o, 8);
  bo_byte(&o, 0);
  bo_byte(&o, 0);
  bo_byte(&o, 0);
  bo_byte(&o, 0);
  bo_byte(&o, 0);
  bo_byte(&o, level >= 9 ? 2 : (level <= 1 ? 4 : 0));
  bo_byte(&o, 3);

  defl.start = 0;
  defl.end = 0;
  for (;;) {
    int want = CHUNK - (defl.end - defl.start);
    int n = 0;
    while (want > 0) {
      int r = read(in, defl.win + defl.end, want);
      if (r < 0)
        return -1;
      if (r == 0)
        break;
      crc = crc32_update(crc, defl.win + defl.end, r);
      total += r;
      defl.end += r;
      want -= r;
      n += r;
    }
    int eof = (want > 0);
    if (defl.end == defl.start && eof) {
      // nothing at all, or nothing left: one empty final block
      defl.ntok = 0;
      for (int i = 0; i < 288; i++)
        defl.lfreq[i] = 0;
      for (int i = 0; i < 30; i++)
        defl.dfreq[i] = 0;
      emit_block(&defl, &o, 1);
      break;
    }
    compress_chunk(&defl, &o, eof);
    if (eof)
      break;
    // keep a window of history and slide the rest down
    int keep = defl.end - WINDOW;
    if (keep < 0)
      keep = 0;
    if (keep > 0) {
      memmove(defl.win, defl.win + keep, defl.end - keep);
      defl.end -= keep;
      // the chain holds positions, and every one of them just moved;
      // build it again over the history that is left
      for (int i = 0; i < HASHSIZE; i++)
        defl.head[i] = NOPOS;
      for (int i = 0; i < POSMAX; i++)
        defl.prev[i] = NOPOS;
      for (int p = 0; p + MINMATCH <= defl.end; p++) {
        int h = hash3(defl.win + p);
        defl.prev[p] = defl.head[h];
        defl.head[h] = p;
      }
    }
    defl.start = defl.end;
  }
  bo_align(&o);
  bo_byte(&o, crc & 0xff);
  bo_byte(&o, (crc >> 8) & 0xff);
  bo_byte(&o, (crc >> 16) & 0xff);
  bo_byte(&o, (crc >> 24) & 0xff);
  uint32_t isize = (uint32_t)(total & 0xffffffffu);
  bo_byte(&o, isize & 0xff);
  bo_byte(&o, (isize >> 8) & 0xff);
  bo_byte(&o, (isize >> 16) & 0xff);
  bo_byte(&o, (isize >> 24) & 0xff);
  bo_flush(&o);
  return o.error ? -1 : 0;
}

// ---- the decompressor ----

struct bitin {
  int fd;
  unsigned char buf[16384];
  int n, pos;
  uint32_t bits;
  int nbits;
  int eof;
};

static int
bi_byte(struct bitin *s)
{
  if (s->pos == s->n) {
    s->n = read(s->fd, s->buf, sizeof(s->buf));
    s->pos = 0;
    if (s->n <= 0) {
      s->n = 0;
      s->eof = 1;
      return -1;
    }
  }
  return s->buf[s->pos++];
}

static int
bi_get(struct bitin *s, int count)
{
  while (s->nbits < count) {
    int c = bi_byte(s);
    if (c < 0)
      return -1;
    s->bits |= (uint32_t)c << s->nbits;
    s->nbits += 8;
  }
  int v = (int)(s->bits & ((1u << count) - 1));
  s->bits >>= count;
  s->nbits -= count;
  return v;
}

static void
bi_align(struct bitin *s)
{
  s->bits >>= (s->nbits & 7);
  s->nbits -= (s->nbits & 7);
}

struct huff {
  short count[16];
  short sym[288];
};

static int
huff_build(struct huff *h, const unsigned char *len, int n)
{
  short offs[16];

  for (int i = 0; i < 16; i++)
    h->count[i] = 0;
  for (int i = 0; i < n; i++)
    h->count[len[i]]++;
  if (h->count[0] == n)
    return 0;
  int left = 1;
  for (int i = 1; i <= 15; i++) {
    left <<= 1;
    left -= h->count[i];
    if (left < 0)
      return left;
  }
  offs[1] = 0;
  for (int i = 1; i < 15; i++)
    offs[i + 1] = offs[i] + h->count[i];
  for (int i = 0; i < n; i++)
    if (len[i])
      h->sym[offs[len[i]]++] = (short)i;
  return left;
}

static int
huff_decode(struct bitin *s, const struct huff *h)
{
  int code = 0, first = 0, index = 0;

  for (int len = 1; len <= 15; len++) {
    int b = bi_get(s, 1);
    if (b < 0)
      return -1;
    code |= b;
    int count = h->count[len];
    if (code - count < first)
      return h->sym[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

struct outbuf {
  int fd;
  unsigned char win[WINDOW];
  int pos;          // where the next byte goes in win
  int64_t total;
  uint32_t crc;
  unsigned char buf[16384];
  int n;
  int error;
};

static void
ob_flush(struct outbuf *ob)
{
  int off = 0;
  while (off < ob->n) {
    int w = write(ob->fd, ob->buf + off, ob->n - off);
    if (w <= 0) {
      ob->error = 1;
      break;
    }
    off += w;
  }
  ob->n = 0;
}

static void
ob_put(struct outbuf *ob, int c)
{
  ob->win[ob->pos] = (unsigned char)c;
  ob->pos = (ob->pos + 1) & (WINDOW - 1);
  ob->buf[ob->n++] = (unsigned char)c;
  ob->total++;
  if (ob->n == sizeof(ob->buf)) {
    ob->crc = crc32_update(ob->crc, ob->buf, ob->n);
    ob_flush(ob);
  }
}

static void
ob_done(struct outbuf *ob)
{
  if (ob->n > 0) {
    ob->crc = crc32_update(ob->crc, ob->buf, ob->n);
    ob_flush(ob);
  }
}

static const unsigned char *fixed_llen(void);
static const unsigned char *fixed_dlen(void);

static int
inflate_block(struct bitin *s, struct outbuf *ob, const struct huff *lh,
              const struct huff *dh)
{
  for (;;) {
    int sym = huff_decode(s, lh);
    if (sym < 0)
      return -1;
    if (sym < 256) {
      ob_put(ob, sym);
    } else if (sym == 256) {
      return 0;
    } else {
      sym -= 257;
      if (sym >= 29)
        return -1;
      int extra = bi_get(s, len_extra[sym]);
      if (extra < 0)
        return -1;
      int len = len_base[sym] + extra;
      int dsym = huff_decode(s, dh);
      if (dsym < 0 || dsym >= 30)
        return -1;
      extra = bi_get(s, dist_extra[dsym]);
      if (extra < 0)
        return -1;
      int dist = dist_base[dsym] + extra;
      if (dist > ob->total)
        return -1; // a reference before the start of the stream
      for (int i = 0; i < len; i++)
        ob_put(ob, ob->win[(ob->pos - dist) & (WINDOW - 1)]);
    }
  }
}

static unsigned char fixed_l[288], fixed_d[30];
static int fixed_ready;

static void
make_fixed(void)
{
  for (int i = 0; i < 144; i++)
    fixed_l[i] = 8;
  for (int i = 144; i < 256; i++)
    fixed_l[i] = 9;
  for (int i = 256; i < 280; i++)
    fixed_l[i] = 7;
  for (int i = 280; i < 288; i++)
    fixed_l[i] = 8;
  for (int i = 0; i < 30; i++)
    fixed_d[i] = 5;
  fixed_ready = 1;
}

static const unsigned char *
fixed_llen(void)
{
  if (!fixed_ready)
    make_fixed();
  return fixed_l;
}

static const unsigned char *
fixed_dlen(void)
{
  if (!fixed_ready)
    make_fixed();
  return fixed_d;
}

static struct huff lh, dh;

static int
inflate(struct bitin *s, struct outbuf *ob)
{
  for (;;) {
    int final = bi_get(s, 1);
    if (final < 0)
      return -1;
    int type = bi_get(s, 2);
    if (type < 0)
      return -1;
    if (type == 0) {
      bi_align(s);
      int a = bi_byte(s), b = bi_byte(s), c = bi_byte(s), d2 = bi_byte(s);
      if (a < 0 || b < 0 || c < 0 || d2 < 0)
        return -1;
      int len = a | (b << 8);
      if ((len ^ 0xffff) != (c | (d2 << 8)))
        return -1;
      for (int i = 0; i < len; i++) {
        int ch = bi_byte(s);
        if (ch < 0)
          return -1;
        ob_put(ob, ch);
      }
    } else if (type == 1) {
      if (huff_build(&lh, fixed_llen(), 288) != 0)
        return -1;
      huff_build(&dh, fixed_dlen(), 30);
      if (inflate_block(s, ob, &lh, &dh) < 0)
        return -1;
    } else if (type == 2) {
      int hlit = bi_get(s, 5);
      int hdist = bi_get(s, 5);
      int hclen = bi_get(s, 4);
      if (hlit < 0 || hdist < 0 || hclen < 0)
        return -1;
      hlit += 257;
      hdist += 1;
      hclen += 4;
      if (hlit > 286 || hdist > 30)
        return -1;
      unsigned char cllen[19];
      for (int i = 0; i < 19; i++)
        cllen[i] = 0;
      for (int i = 0; i < hclen; i++) {
        int v = bi_get(s, 3);
        if (v < 0)
          return -1;
        cllen[clorder[i]] = (unsigned char)v;
      }
      struct huff ch;
      if (huff_build(&ch, cllen, 19) != 0)
        return -1;
      unsigned char both[288 + 30];
      int i = 0;
      while (i < hlit + hdist) {
        int sym = huff_decode(s, &ch);
        if (sym < 0)
          return -1;
        if (sym < 16) {
          both[i++] = (unsigned char)sym;
        } else if (sym == 16) {
          if (i == 0)
            return -1;
          int n = bi_get(s, 2);
          if (n < 0)
            return -1;
          n += 3;
          unsigned char prev = both[i - 1];
          while (n-- > 0 && i < hlit + hdist)
            both[i++] = prev;
        } else {
          int n;
          if (sym == 17) {
            n = bi_get(s, 3);
            if (n < 0)
              return -1;
            n += 3;
          } else {
            n = bi_get(s, 7);
            if (n < 0)
              return -1;
            n += 11;
          }
          while (n-- > 0 && i < hlit + hdist)
            both[i++] = 0;
        }
      }
      if (huff_build(&lh, both, hlit) != 0)
        return -1;
      // an incomplete distance code is allowed when nothing uses it
      unsigned char dl[30];
      for (int k = 0; k < 30; k++)
        dl[k] = k < hdist ? both[hlit + k] : 0;
      int err = huff_build(&dh, dl, hdist);
      if (err < 0)
        return -1;
      if (inflate_block(s, ob, &lh, &dh) < 0)
        return -1;
    } else {
      return -1;
    }
    if (final)
      return 0;
  }
}

static int
decompress(int in, int out, const char *what)
{
  static struct bitin s;
  static struct outbuf ob;

  memset(&s, 0, sizeof(s));
  memset(&ob, 0, sizeof(ob));
  s.fd = in;
  ob.fd = out;

  int m1 = bi_byte(&s), m2 = bi_byte(&s), cm = bi_byte(&s), flg = bi_byte(&s);
  if (m1 != 0x1f || m2 != 0x8b) {
    fprintf(stderr, "gzip: %s: not in gzip format\n", what);
    return -1;
  }
  if (cm != 8) {
    fprintf(stderr, "gzip: %s: unknown compression method %d\n", what, cm);
    return -1;
  }
  for (int i = 0; i < 6; i++)
    bi_byte(&s); // mtime, xfl, os
  if (flg & 4) { // an extra field
    int a = bi_byte(&s), b = bi_byte(&s);
    int n = a | (b << 8);
    while (n-- > 0)
      bi_byte(&s);
  }
  if (flg & 8)   // the original name
    while (bi_byte(&s) > 0)
      ;
  if (flg & 16)  // a comment
    while (bi_byte(&s) > 0)
      ;
  if (flg & 2) { // a header check
    bi_byte(&s);
    bi_byte(&s);
  }
  if (s.eof) {
    fprintf(stderr, "gzip: %s: the file ends inside its header\n", what);
    return -1;
  }

  if (inflate(&s, &ob) < 0) {
    ob_done(&ob);
    fprintf(stderr, "gzip: %s: the compressed data is damaged\n", what);
    return -1;
  }
  ob_done(&ob);
  if (ob.error)
    return -1;

  bi_align(&s);
  uint32_t crc = 0, isize = 0;
  for (int i = 0; i < 4; i++) {
    int c = bi_get(&s, 8);
    if (c < 0)
      c = 0;
    crc |= (uint32_t)c << (i * 8);
  }
  for (int i = 0; i < 4; i++) {
    int c = bi_get(&s, 8);
    if (c < 0)
      c = 0;
    isize |= (uint32_t)c << (i * 8);
  }
  if (crc != ob.crc) {
    fprintf(stderr, "gzip: %s: the checksum does not match\n", what);
    return -1;
  }
  if (isize != (uint32_t)(ob.total & 0xffffffffu)) {
    fprintf(stderr, "gzip: %s: the length does not match\n", what);
    return -1;
  }
  return 0;
}

// ---- the program around it ----

static int to_stdout, decomp, keep, force, quiet, verbose, level = 6;
static int status;

static void
one_file(const char *path)
{
  char out[PATH_MAX];
  struct stat st;

  if (stat(path, &st) < 0) {
    fprintf(stderr, "gzip: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  if (st.type == T_DIR) {
    fprintf(stderr, "gzip: %s: is a directory\n", path);
    status = 1;
    return;
  }
  int n = strlen(path);
  int is_gz = n > 3 && strcmp(path + n - 3, ".gz") == 0;
  if (decomp) {
    if (!is_gz && !to_stdout) {
      fprintf(stderr, "gzip: %s: unknown suffix -- ignored\n", path);
      status = 1;
      return;
    }
    snprintf(out, sizeof(out), "%.*s", is_gz ? n - 3 : n, path);
  } else {
    if (is_gz && !to_stdout) {
      fprintf(stderr, "gzip: %s already has .gz -- ignored\n", path);
      status = 1;
      return;
    }
    snprintf(out, sizeof(out), "%s.gz", path);
  }

  int in = open(path, O_RDONLY);
  if (in < 0) {
    fprintf(stderr, "gzip: %s: %s\n", path, strerror(errno));
    status = 1;
    return;
  }
  int outfd = 1;
  if (!to_stdout) {
    if (!force && stat(out, &st) >= 0) {
      fprintf(stderr, "gzip: %s already exists; not overwritten\n", out);
      close(in);
      status = 1;
      return;
    }
    outfd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
      fprintf(stderr, "gzip: %s: %s\n", out, strerror(errno));
      close(in);
      status = 1;
      return;
    }
  }

  int r = decomp ? decompress(in, outfd, path) : compress(in, outfd, level);
  close(in);
  if (outfd != 1)
    close(outfd);
  if (r < 0) {
    status = 1;
    if (outfd != 1)
      unlink(out);
    return;
  }
  if (verbose && !to_stdout) {
    struct stat a, b;
    if (stat(path, &a) >= 0 && stat(out, &b) >= 0 && a.size > 0)
      fprintf(stderr, "%s:\t%lld%% -- replaced with %s\n", path,
              decomp ? 0LL : 100LL - (long long)b.size * 100 / (long long)a.size,
              out);
  }
  if (!to_stdout && !keep)
    unlink(path);
}

int
main(int argc, char *argv[])
{
  const char *base = argv[0];
  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      base = p + 1;
  if (strcmp(base, "gunzip") == 0) {
    decomp = 1;
  } else if (strcmp(base, "zcat") == 0) {
    decomp = 1;
    to_stdout = 1;
    keep = 1;
  }

  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (const char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'c': to_stdout = 1; keep = 1; break;
      case 'd': decomp = 1; break;
      case 'k': keep = 1; break;
      case 'f': force = 1; break;
      case 'q': quiet = 1; break;
      case 'v': verbose = 1; break;
      case 'n': break;
      case '1': case '2': case '3': case '4': case '5':
      case '6': case '7': case '8': case '9':
        level = *p - '0';
        break;
      default:
        fprintf(stderr, "gzip: unknown option -%c\n", *p);
        exit(1);
      }
    }
  }
  (void)quiet;

  if (i >= argc) {
    int r = decomp ? decompress(0, 1, "stdin") : compress(0, 1, level);
    exit(r < 0 ? 1 : 0);
  }
  for (; i < argc; i++)
    one_file(argv[i]);
  exit(status);
}
