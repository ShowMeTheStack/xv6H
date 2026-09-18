//
// minimal flattened device tree (FDT / DTB) reader.
// runs very early (M mode, no paging, no locks); uses no kernel services.
//

#include "types.h"
#include "fdt.h"

uint32
fdt32(const void *p)
{
  const unsigned char *b = p;
  return ((uint32)b[0] << 24) | ((uint32)b[1] << 16) | ((uint32)b[2] << 8) | b[3];
}

uint64
fdt64(const void *p)
{
  return ((uint64)fdt32(p) << 32) | fdt32((const char *)p + 4);
}

int
fdt_valid(const void *fdt)
{
  if (fdt == 0 || ((uint64)fdt & 3) != 0)
    return 0;
  if (fdt32(fdt) != FDT_MAGIC)
    return 0;
  uint32 total = fdt32((const char *)fdt + 4);
  if (total < 40 || total > 1024 * 1024)
    return 0;
  return 1;
}

uint32
fdt_totalsize(const void *fdt)
{
  return fdt32((const char *)fdt + 4);
}

void
fdt_iter_init(struct fdt_iter *it, const void *fdt)
{
  const char *b = fdt;
  it->base = b;
  it->pos = fdt32(b + 8);           // off_dt_struct
  it->strings = fdt32(b + 12);      // off_dt_strings
  it->end = it->pos + fdt32(b + 36); // size_dt_struct
  it->depth = -1;
}

static int
fdt_strlen(const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  return n;
}

// advance to the next BEGIN_NODE / END_NODE / PROP token.
// returns 0 when the tree is exhausted.
int
fdt_iter_next(struct fdt_iter *it, struct fdt_item *item)
{
  while (it->pos + 4 <= it->end) {
    uint32 tok = fdt32(it->base + it->pos);
    it->pos += 4;
    switch (tok) {
    case FDT_BEGIN_NODE: {
      const char *name = it->base + it->pos;
      int n = fdt_strlen(name);
      it->pos += (n + 1 + 3) & ~3;
      it->depth++;
      item->tok = FDT_BEGIN_NODE;
      item->depth = it->depth;
      item->name = name;
      item->prop = 0;
      item->data = 0;
      item->len = 0;
      return 1;
    }
    case FDT_END_NODE:
      item->tok = FDT_END_NODE;
      item->depth = it->depth;
      item->name = 0;
      item->prop = 0;
      item->data = 0;
      item->len = 0;
      it->depth--;
      return 1;
    case FDT_PROP: {
      uint32 len = fdt32(it->base + it->pos);
      uint32 nameoff = fdt32(it->base + it->pos + 4);
      it->pos += 8;
      item->tok = FDT_PROP;
      item->depth = it->depth;
      item->name = 0;
      item->prop = it->base + it->strings + nameoff;
      item->data = it->base + it->pos;
      item->len = len;
      it->pos += (len + 3) & ~3;
      return 1;
    }
    case FDT_NOP:
      continue;
    case FDT_END:
    default:
      it->pos = it->end;
      return 0;
    }
  }
  return 0;
}

// does a NUL-separated string list property contain s?
int
fdt_strlist_has(const void *data, uint32 len, const char *s)
{
  const char *p = data;
  uint32 i = 0;
  while (i < len) {
    const char *q = p + i;
    int n = fdt_strlen(q);
    int k;
    for (k = 0; k <= n; k++) {
      if (q[k] != s[k])
        break;
    }
    if (k == n + 1)
      return 1;
    i += n + 1;
  }
  return 0;
}

// does node name start with prefix (e.g. "memory", "cpu@")?
int
fdt_name_is(const char *name, const char *prefix)
{
  while (*prefix) {
    if (*name != *prefix)
      return 0;
    name++;
    prefix++;
  }
  return 1;
}
