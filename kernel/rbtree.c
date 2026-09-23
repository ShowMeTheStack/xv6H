//
// a red-black tree (see rbtree.h). The algorithms are the ones in
// Cormen, Leiserson, Rivest and Stein, with a null sentinel spelled
// out as 0 rather than a shared leaf node, and with the leftmost node
// kept alongside the root so that the scheduler's commonest question
// -- who has the least virtual time? -- is a field read.
//
#include "types.h"
#include "rbtree.h"

static void
rotate_left(struct rb_root *t, struct rb_node *x)
{
  struct rb_node *y = x->right;

  x->right = y->left;
  if (y->left)
    y->left->parent = x;
  y->parent = x->parent;
  if (x->parent == 0)
    t->root = y;
  else if (x == x->parent->left)
    x->parent->left = y;
  else
    x->parent->right = y;
  y->left = x;
  x->parent = y;
}

static void
rotate_right(struct rb_root *t, struct rb_node *x)
{
  struct rb_node *y = x->left;

  x->left = y->right;
  if (y->right)
    y->right->parent = x;
  y->parent = x->parent;
  if (x->parent == 0)
    t->root = y;
  else if (x == x->parent->right)
    x->parent->right = y;
  else
    x->parent->left = y;
  y->right = x;
  x->parent = y;
}

void
rb_insert(struct rb_root *t, struct rb_node *n, uint64 key)
{
  struct rb_node *parent = 0, **link = &t->root;
  int leftmost = 1;

  n->key = key;
  n->left = n->right = 0;
  n->red = 1;

  while (*link) {
    parent = *link;
    // an equal key goes to the right, so that a task does not jump
    // ahead of one that has been waiting with the same virtual time
    if (key < parent->key) {
      link = &parent->left;
    } else {
      link = &parent->right;
      leftmost = 0;
    }
  }
  n->parent = parent;
  *link = n;
  if (leftmost)
    t->leftmost = n;
  t->count++;

  // restore the properties: a red node's parent must be black
  struct rb_node *x = n;
  while (x->parent && x->parent->red) {
    struct rb_node *p = x->parent, *g = p->parent;
    if (g == 0)
      break;
    if (p == g->left) {
      struct rb_node *u = g->right;
      if (u && u->red) {
        p->red = 0;
        u->red = 0;
        g->red = 1;
        x = g;
      } else {
        if (x == p->right) {
          x = p;
          rotate_left(t, x);
          p = x->parent;
          g = p->parent;
        }
        p->red = 0;
        g->red = 1;
        rotate_right(t, g);
      }
    } else {
      struct rb_node *u = g->left;
      if (u && u->red) {
        p->red = 0;
        u->red = 0;
        g->red = 1;
        x = g;
      } else {
        if (x == p->left) {
          x = p;
          rotate_right(t, x);
          p = x->parent;
          g = p->parent;
        }
        p->red = 0;
        g->red = 1;
        rotate_left(t, g);
      }
    }
  }
  t->root->red = 0;
}

static struct rb_node *
minimum(struct rb_node *n)
{
  while (n->left)
    n = n->left;
  return n;
}

// the greatest key: the one that would run last
struct rb_node *
rb_last(struct rb_root *t)
{
  struct rb_node *n = t->root;

  if (n == 0)
    return 0;
  while (n->right)
    n = n->right;
  return n;
}

struct rb_node *
rb_next(struct rb_node *n)
{
  if (n->right)
    return minimum(n->right);
  struct rb_node *p = n->parent;
  while (p && n == p->right) {
    n = p;
    p = p->parent;
  }
  return p;
}

// and the one before it
struct rb_node *
rb_prev(struct rb_node *n)
{
  if (n->left) {
    n = n->left;
    while (n->right)
      n = n->right;
    return n;
  }
  struct rb_node *p = n->parent;
  while (p && n == p->left) {
    n = p;
    p = p->parent;
  }
  return p;
}

// the fixup after a black node has been taken out. `x` may be 0, so
// its parent is passed alongside it -- the one place where a null
// sentinel would have saved a parameter.
static void
erase_fixup(struct rb_root *t, struct rb_node *x, struct rb_node *parent)
{
  while (x != t->root && (x == 0 || !x->red)) {
    if (x == parent->left) {
      struct rb_node *w = parent->right;
      if (w && w->red) {
        w->red = 0;
        parent->red = 1;
        rotate_left(t, parent);
        w = parent->right;
      }
      if (w == 0)
        break;
      if ((w->left == 0 || !w->left->red) && (w->right == 0 || !w->right->red)) {
        w->red = 1;
        x = parent;
        parent = x->parent;
      } else {
        if (w->right == 0 || !w->right->red) {
          if (w->left)
            w->left->red = 0;
          w->red = 1;
          rotate_right(t, w);
          w = parent->right;
        }
        w->red = parent->red;
        parent->red = 0;
        if (w->right)
          w->right->red = 0;
        rotate_left(t, parent);
        x = t->root;
        parent = 0;
      }
    } else {
      struct rb_node *w = parent->left;
      if (w && w->red) {
        w->red = 0;
        parent->red = 1;
        rotate_right(t, parent);
        w = parent->left;
      }
      if (w == 0)
        break;
      if ((w->left == 0 || !w->left->red) && (w->right == 0 || !w->right->red)) {
        w->red = 1;
        x = parent;
        parent = x->parent;
      } else {
        if (w->left == 0 || !w->left->red) {
          if (w->right)
            w->right->red = 0;
          w->red = 1;
          rotate_left(t, w);
          w = parent->left;
        }
        w->red = parent->red;
        parent->red = 0;
        if (w->left)
          w->left->red = 0;
        rotate_right(t, parent);
        x = t->root;
        parent = 0;
      }
    }
  }
  if (x)
    x->red = 0;
}

void
rb_erase(struct rb_root *t, struct rb_node *n)
{
  struct rb_node *child, *parent;
  int red;

  if (t->leftmost == n)
    t->leftmost = rb_next(n);
  t->count--;

  if (n->left && n->right) {
    // two children: stand the successor in its place, physically, so
    // that no key is copied -- the node belongs to somebody else and
    // its address is what identifies it
    struct rb_node *s = minimum(n->right);
    struct rb_node *sp = s->parent;
    child = s->right;
    red = s->red;
    if (sp == n) {
      parent = s;
    } else {
      parent = sp;
      sp->left = child;
      if (child)
        child->parent = sp;
      s->right = n->right;
      n->right->parent = s;
    }
    s->left = n->left;
    n->left->parent = s;
    s->parent = n->parent;
    s->red = n->red;
    if (n->parent == 0)
      t->root = s;
    else if (n->parent->left == n)
      n->parent->left = s;
    else
      n->parent->right = s;
  } else {
    child = n->left ? n->left : n->right;
    parent = n->parent;
    red = n->red;
    if (child)
      child->parent = parent;
    if (parent == 0)
      t->root = child;
    else if (parent->left == n)
      parent->left = child;
    else
      parent->right = child;
  }

  n->left = n->right = n->parent = 0;
  if (!red)
    erase_fixup(t, child, parent);
}

// ---- the self-test's half ----

// the black height below n, or -1 if a property fails
static int
check(struct rb_node *n, struct rb_node *parent, uint64 lo, uint64 hi)
{
  if (n == 0)
    return 1;
  if (n->parent != parent)
    return -1;
  if (n->key < lo || n->key > hi)
    return -1;
  if (n->red && ((n->left && n->left->red) || (n->right && n->right->red)))
    return -1;
  int l = check(n->left, n, lo, n->key);
  int r = check(n->right, n, n->key, hi);
  if (l < 0 || r < 0 || l != r)
    return -1;
  return l + (n->red ? 0 : 1);
}

int
rb_check(struct rb_root *t)
{
  if (t->root == 0)
    return t->leftmost == 0 && t->count == 0 ? 0 : -1;
  if (t->root->red)
    return -1;
  if (check(t->root, 0, 0, ~0UL) < 0)
    return -1;
  return t->leftmost == minimum(t->root) ? 0 : -1;
}
