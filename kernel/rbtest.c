//
// the red-black tree's self-test.
//
// a tree is the kind of thing that is either right or quietly wrong:
// a delete that breaks a black height does not crash, it just makes
// the scheduler pick the wrong task now and then, for ever. So this
// runs at boot, builds and tears down a tree of a few thousand keys
// in a deliberately awkward order, and checks after every step that
// every red-black property still holds and that the order is right.
//
#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "rbtree.h"

#define N 512

struct item {
  struct rb_node node;
  uint64 key;
  int in;
};

static struct item items[N];
static struct rb_root tree;

// a cheap deterministic shuffle: the same every boot, so a failure is
// the same failure twice
static uint32
rnd(uint32 *s)
{
  *s ^= *s << 13;
  *s ^= *s >> 17;
  *s ^= *s << 5;
  return *s;
}

static int
ordered(void)
{
  struct rb_node *n = rb_first(&tree);
  uint64 last = 0;
  int seen = 0;

  for (; n; n = rb_next(n)) {
    if (seen && n->key < last)
      return -1;
    last = n->key;
    seen++;
  }
  if (seen != tree.count)
    return -1;

  // and backwards from the far end: rb_prev is what the idle hart
  // walks when it is looking for a task it is allowed to steal, so it
  // has to give the same sequence in the other direction.
  int back = 0;
  n = rb_last(&tree);
  for (; n; n = rb_prev(n)) {
    if (back && n->key > last)
      return -1;
    last = n->key;
    back++;
  }
  return back == tree.count ? 0 : -1;
}

void
rbtest(void)
{
  uint32 s = 12345;
  int fails = 0;

  memset(&tree, 0, sizeof(tree));
  memset(items, 0, sizeof(items));

  // keys with many duplicates, so the equal-key case is exercised
  for (int i = 0; i < N; i++)
    items[i].key = rnd(&s) % (N / 4);

  for (int i = 0; i < N; i++) {
    rb_insert(&tree, &items[i].node, items[i].key);
    items[i].in = 1;
    if (rb_check(&tree) < 0 || ordered() < 0)
      fails++;
  }
  if (tree.count != N)
    fails++;

  // delete in a scattered order, checking as we go
  for (int round = 0; round < 3; round++) {
    for (int i = round; i < N; i += 3) {
      if (!items[i].in)
        continue;
      rb_erase(&tree, &items[i].node);
      items[i].in = 0;
      if (rb_check(&tree) < 0 || ordered() < 0)
        fails++;
    }
    // and put some back, so the tree is rebuilt from a used shape
    for (int i = round; i < N; i += 6) {
      if (items[i].in)
        continue;
      rb_insert(&tree, &items[i].node, items[i].key);
      items[i].in = 1;
      if (rb_check(&tree) < 0 || ordered() < 0)
        fails++;
    }
  }

  // the minimum is always the least key
  for (int i = 0; i < N; i++) {
    if (!items[i].in)
      continue;
    struct rb_node *m = rb_first(&tree);
    if (m == 0 || m->key > items[i].key)
      fails++;
  }

  // empty it
  for (int i = 0; i < N; i++) {
    if (!items[i].in)
      continue;
    rb_erase(&tree, &items[i].node);
    items[i].in = 0;
    if (rb_check(&tree) < 0)
      fails++;
  }
  if (!rb_empty(&tree) || tree.count != 0 || rb_first(&tree) != 0)
    fails++;

  printk("rbtree: %s\n", fails == 0 ? "ok" : "FAILED");
  if (fails)
    panic("rbtree self-test");
}
