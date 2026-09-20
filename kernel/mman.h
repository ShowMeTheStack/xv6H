#ifndef XV6_MMAN_H
#define XV6_MMAN_H
// mmap(2) constants (the values match the areas' prot bits).
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

#endif
