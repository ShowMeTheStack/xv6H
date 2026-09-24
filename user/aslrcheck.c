// aslrcheck: print the addresses this run was given, one per line, so
// that two runs can be compared. Nothing here is a test on its own --
// the test is that the numbers move when randomisation is on and do
// not when it is off (t_aslr in test-hyp.py).
#include "kernel/types.h"
#include "user/user.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int
main(void)
{
  char local;                       // somewhere on the stack
  void *heap = malloc(64);          // this libc's malloc maps its own
  void *brk = sbrk(0);              // the heap the kernel gives, unused
  void *map = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  // main() is in this program's own text; printf() is in libc, which
  // the loader maps out of the mmap area when the program is dynamic.
  printf("text=0x%lx libc=0x%lx stack=0x%lx heap=0x%lx mmap=0x%lx brk=0x%lx\n",
         (unsigned long)(uint64)(void *)main, (unsigned long)(uint64)(void *)printf,
         (unsigned long)(uint64)&local, (unsigned long)(uint64)heap,
         (unsigned long)(uint64)map, (unsigned long)(uint64)brk);
  return 0;
}
