#ifndef XV6_RBTREE_H
#define XV6_RBTREE_H
//
// a red-black tree, ordered by a 64-bit key.
//
// the scheduler wants the runnable task with the least virtual time,
// again and again, while tasks come and go at every tick: that is a
// sorted set with cheap insert, cheap delete and a cheap minimum, and
// a red-black tree is the usual answer. The node is embedded in
// whatever is being ordered, so nothing is allocated to schedule.
//
// the leftmost node is cached, because picking the next task to run
// asks for it every time and walking down the left spine to find it
// would be the one O(log n) that did not have to be.
//
// keys need not be distinct. A node inserted with a key equal to
// another goes after it, which keeps the order total and stops two
// tasks with the same virtual time from starving each other.
//
#include "types.h"

struct rb_node {
  struct rb_node *left, *right, *parent;
  uint64 key;
  int red;
  // whatever the owner needs to say about what this node is embedded
  // in. The scheduler's trees hold two kinds of thing -- a task, and
  // a group of tasks standing in for all of them -- and this is how
  // it tells them apart. The tree itself never looks at it.
  int tag;
};

struct rb_root {
  struct rb_node *root;
  struct rb_node *leftmost;
  int count;
};

void rb_insert(struct rb_root *t, struct rb_node *n, uint64 key);
void rb_erase(struct rb_root *t, struct rb_node *n);
static inline struct rb_node *rb_first(struct rb_root *t) { return t->leftmost; }
static inline int rb_empty(struct rb_root *t) { return t->root == 0; }
struct rb_node *rb_next(struct rb_node *n);
struct rb_node *rb_prev(struct rb_node *n);
struct rb_node *rb_last(struct rb_root *t);

// for the self-test: 0 if every red-black property holds
int rb_check(struct rb_root *t);
#endif
