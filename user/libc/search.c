//
// <search.h>: the four small table-and-tree interfaces POSIX keeps
// here, which are older than the rest of the library and show it.
//
// the hash table is open-addressed with linear probing, kept at most
// half full, because that needs one allocation and no chains; hsearch
// may not grow the table anyway, so the size is decided once and a
// table that fills up says so rather than degrading.
//
// the binary tree is a plain unbalanced search tree. POSIX does not
// require balance, and neither tsearch nor tdelete can rebalance
// behind the caller's back without invalidating the node pointers it
// has already handed out. keys arriving in sorted order therefore give
// a linear tree; a caller that expects that should shuffle or use the
// hash table instead.
//
#include <search.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

// ---- the hash table ----

struct __hslot {
  int used;
  ENTRY entry;
};

// FNV-1a: one multiply and one exclusive-or per byte, and it scatters
// short keys, which are what this table is usually asked to hold.
static size_t
hashkey(const char *s)
{
  size_t h = 1469598103934665603UL;

  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 1099511628211UL;
  }
  return h;
}

int
hcreate_r(size_t nel, struct hsearch_data *tab)
{
  size_t size = 8;

  if (tab == 0) {
    errno = EINVAL;
    return 0;
  }
  if (tab->table) {
    errno = EINVAL; // a table already exists here
    return 0;
  }
  // twice the requested entries, rounded up to a power of two, so the
  // probe sequence stays short and the index is a mask rather than a
  // remainder.
  while (size < nel * 2)
    size *= 2;
  tab->table = calloc(size, sizeof(*tab->table));
  if (tab->table == 0)
    return 0;
  tab->size = size;
  tab->filled = 0;
  return 1;
}

void
hdestroy_r(struct hsearch_data *tab)
{
  if (tab == 0)
    return;
  // the keys and the data belong to the caller: POSIX says the table
  // holds the pointers it was given and frees none of them.
  free(tab->table);
  tab->table = 0;
  tab->size = tab->filled = 0;
}

int
hsearch_r(ENTRY item, ACTION action, ENTRY **ret, struct hsearch_data *tab)
{
  if (tab == 0 || tab->table == 0) {
    errno = EINVAL;
    return 0;
  }
  size_t mask = tab->size - 1;
  size_t i = hashkey(item.key) & mask;
  for (size_t probe = 0; probe < tab->size; probe++) {
    struct __hslot *s = &tab->table[i];
    if (!s->used) {
      if (action == FIND) {
        errno = ESRCH;
        return 0;
      }
      if (tab->filled * 2 >= tab->size) {
        errno = ENOMEM; // a fuller table would probe for too long
        return 0;
      }
      s->used = 1;
      s->entry = item;
      tab->filled++;
      *ret = &s->entry;
      return 1;
    }
    if (strcmp(s->entry.key, item.key) == 0) {
      // ENTER on a key already there returns the old entry unchanged
      *ret = &s->entry;
      return 1;
    }
    i = (i + 1) & mask;
  }
  errno = ENOMEM;
  return 0;
}

// the one table hcreate() manages, since that interface has nowhere to
// put a handle.
static struct hsearch_data htab;

int
hcreate(size_t nel)
{
  return hcreate_r(nel, &htab);
}

void
hdestroy(void)
{
  hdestroy_r(&htab);
}

ENTRY *
hsearch(ENTRY item, ACTION action)
{
  ENTRY *ret = 0;

  if (!hsearch_r(item, action, &ret, &htab))
    return 0;
  return ret;
}

// ---- the binary search tree ----

// the key comes first so that the node pointer tsearch returns is also
// a pointer to the key pointer, which is what the caller dereferences.
struct tnode {
  const void *key;
  struct tnode *left, *right;
};

void *
tsearch(const void *key, void **rootp, int (*cmp)(const void *, const void *))
{
  if (rootp == 0)
    return 0;

  struct tnode **p = (struct tnode **)rootp;
  while (*p) {
    int c = cmp(key, (*p)->key);
    if (c == 0)
      return *p;
    p = c < 0 ? &(*p)->left : &(*p)->right;
  }
  struct tnode *n = malloc(sizeof(*n));
  if (n == 0)
    return 0;
  n->key = key; // the key is the caller's; the tree only points at it
  n->left = n->right = 0;
  *p = n;
  return n;
}

void *
tfind(const void *key, void *const *rootp, int (*cmp)(const void *, const void *))
{
  if (rootp == 0)
    return 0;

  struct tnode *n = *(struct tnode *const *)rootp;
  while (n) {
    int c = cmp(key, n->key);
    if (c == 0)
      return n;
    n = c < 0 ? n->left : n->right;
  }
  return 0;
}

void *
tdelete(const void *key, void **rootp, int (*cmp)(const void *, const void *))
{
  if (rootp == 0 || *rootp == 0)
    return 0;

  struct tnode **link = (struct tnode **)rootp;
  struct tnode *parent = 0;
  while (*link) {
    int c = cmp(key, (*link)->key);
    if (c == 0)
      break;
    parent = *link;
    link = c < 0 ? &(*link)->left : &(*link)->right;
  }
  if (*link == 0)
    return 0;

  struct tnode *n = *link;
  if (n->left && n->right) {
    // two children: take the smallest key of the right subtree, which
    // has no left child, and put it where this node was.
    struct tnode **sp = &n->right;
    while ((*sp)->left)
      sp = &(*sp)->left;
    struct tnode *succ = *sp;
    *sp = succ->right;
    succ->left = n->left;
    succ->right = n->right;
    *link = succ;
  } else {
    *link = n->left ? n->left : n->right;
  }
  free(n);
  // POSIX wants the parent of what was removed; when the root went,
  // there is no parent, so the root pointer itself stands in for one
  // and tells the caller apart from "not found".
  return parent ? (void *)parent : (void *)rootp;
}

static void
walk(const struct tnode *n, void (*action)(const void *, VISIT, int), int depth)
{
  if (n == 0)
    return;
  if (n->left == 0 && n->right == 0) {
    action(n, leaf, depth);
    return;
  }
  action(n, preorder, depth);
  walk(n->left, action, depth + 1);
  action(n, postorder, depth);
  walk(n->right, action, depth + 1);
  action(n, endorder, depth);
}

void
twalk(const void *root, void (*action)(const void *, VISIT, int))
{
  if (root && action)
    walk(root, action, 0);
}

void
tdestroy(void *root, void (*freekey)(void *))
{
  struct tnode *n = root;

  if (n == 0)
    return;
  tdestroy(n->left, freekey);
  tdestroy(n->right, freekey);
  if (freekey)
    freekey((void *)n->key);
  free(n);
}

// ---- linear search over a plain array ----

void *
lfind(const void *key, const void *base, size_t *nelp, size_t width,
      int (*cmp)(const void *, const void *))
{
  const char *p = base;

  for (size_t i = 0; i < *nelp; i++, p += width)
    if (cmp(key, p) == 0)
      return (void *)p;
  return 0;
}

void *
lsearch(const void *key, void *base, size_t *nelp, size_t width,
        int (*cmp)(const void *, const void *))
{
  void *p = lfind(key, base, nelp, width, cmp);

  if (p)
    return p;
  // not there: the caller promised room for one more past the end.
  p = (char *)base + *nelp * width;
  memcpy(p, key, width);
  (*nelp)++;
  return p;
}

// ---- the two-pointer queues ----

// the elements of one of these begin with two pointers and are
// otherwise the caller's business, so this is the whole type.
struct qelem {
  struct qelem *next, *prev;
};

void
insque(void *elem, void *pred)
{
  struct qelem *e = elem, *p = pred;

  if (p == 0) {
    // the first element of a linear queue: it points at nothing.
    e->next = e->prev = 0;
    return;
  }
  e->next = p->next;
  e->prev = p;
  if (p->next)
    p->next->prev = e;
  p->next = e;
}

void
remque(void *elem)
{
  struct qelem *e = elem;

  if (e->prev)
    e->prev->next = e->next;
  if (e->next)
    e->next->prev = e->prev;
  e->next = e->prev = 0;
}
