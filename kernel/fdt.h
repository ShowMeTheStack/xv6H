//
// minimal flattened device tree (FDT / DTB) reader.
// only what platform.c needs: a token iterator plus a few helpers.
//

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

struct fdt_iter {
  const char *base;
  uint32 pos;     // current offset into structure block
  uint32 end;     // end of structure block
  uint32 strings; // offset of strings block
  int depth;      // depth of the node most recently begun
};

struct fdt_item {
  int tok;          // FDT_BEGIN_NODE, FDT_END_NODE, or FDT_PROP
  int depth;        // node depth (root = 0)
  const char *name; // node name (BEGIN_NODE)
  const char *prop; // property name (PROP)
  const void *data; // property value (PROP)
  uint32 len;       // property value length (PROP)
};

uint32 fdt32(const void *p);
uint64 fdt64(const void *p);
int fdt_valid(const void *fdt);
uint32 fdt_totalsize(const void *fdt);
void fdt_iter_init(struct fdt_iter *it, const void *fdt);
int fdt_iter_next(struct fdt_iter *it, struct fdt_item *item);
int fdt_strlist_has(const void *data, uint32 len, const char *s);
int fdt_name_is(const char *name, const char *prefix);
