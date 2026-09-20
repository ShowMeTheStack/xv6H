#ifndef _SEARCH_H
#define _SEARCH_H
#include <sys/cdefs.h>
#include <stddef.h>

typedef enum { FIND, ENTER } ACTION;
typedef enum { preorder, postorder, endorder, leaf } VISIT;

typedef struct entry {
  char *key;
  void *data;
} ENTRY;

// a slot of the hash table. the type stays incomplete here because
// nothing outside search.c has any business looking into one.
struct __hslot;

struct hsearch_data {
  struct __hslot *table;
  size_t size;   // slots, always a power of two
  size_t filled; // slots in use, so the table can refuse to fill up
};

int hcreate(size_t nel);
void hdestroy(void);
ENTRY *hsearch(ENTRY item, ACTION action);
int hcreate_r(size_t nel, struct hsearch_data *tab);
void hdestroy_r(struct hsearch_data *tab);
int hsearch_r(ENTRY item, ACTION action, ENTRY **ret, struct hsearch_data *tab);

void *tsearch(const void *key, void **rootp, int (*cmp)(const void *, const void *));
void *tfind(const void *key, void *const *rootp, int (*cmp)(const void *, const void *));
void *tdelete(const void *key, void **rootp, int (*cmp)(const void *, const void *));
void twalk(const void *root, void (*action)(const void *, VISIT, int));
void tdestroy(void *root, void (*freekey)(void *));

void *lsearch(const void *key, void *base, size_t *nelp, size_t width,
              int (*cmp)(const void *, const void *));
void *lfind(const void *key, const void *base, size_t *nelp, size_t width,
            int (*cmp)(const void *, const void *));
void insque(void *elem, void *pred);
void remque(void *elem);
#endif
