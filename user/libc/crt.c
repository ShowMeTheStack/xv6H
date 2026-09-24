//
// what runs before and around main().
//
// the kernel starts a program at start(), with argc, argv and the
// environment where exec left them, and the auxiliary vector above
// them. before anything else can fail, this gives the first thread a
// thread pointer, because errno lives below it and every system call
// stub writes there.
//
// the thread pointer points at the thread's static TLS block: the
// __thread variables of the program and of the library live at
// positive offsets from it, and the control block at negative ones.
// the initial contents of that block come from the PT_TLS program
// header, which the auxiliary vector leads us to.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include "kernel/elf.h"
#include "kernel/posix.h"
#include "libc.h"

char **environ;
char *program_invocation_name = "";
char *program_invocation_short_name = "";

// what the program's PT_TLS says: where the initialized part lives,
// how much of it there is, and how much zero follows.
void *__tls_image;
size_t __tls_filesz, __tls_memsz, __tls_align;

// room for the first thread's control block and a small TLS block, so
// that a program with no thread-local data allocates nothing at all.
#define STATIC_TLS 512
static char main_block[STATIC_TLS + sizeof(struct __tcb)] __attribute__((aligned(16)));

int *
__errno_location(void)
{
  char *tp;

  asm volatile("mv %0, tp" : "=r"(tp));
  return (int *)(tp + TCB_ERRNO_OFF);
}

// lay a thread's TLS block and control block out in `mem`, which must
// hold __tls_size() bytes, and return the thread pointer for it.
size_t
__tls_size(void)
{
  size_t n = __tls_memsz + sizeof(struct __tcb);

  return (n + 15) & ~(size_t)15;
}

void *
__tls_init(void *mem)
{
  char *tp = (char *)mem + sizeof(struct __tcb);
  size_t align = __tls_align > 16 ? __tls_align : 16;

  // the TLS block must be aligned as the program asked
  tp = (char *)(((unsigned long)tp + align - 1) & ~(unsigned long)(align - 1));
  struct __tcb *tcb = (struct __tcb *)(tp - sizeof(struct __tcb));
  tcb->self = tcb;
  tcb->tid = 0;
  tcb->err = 0;
  if (__tls_filesz)
    memcpy(tp, __tls_image, __tls_filesz);
  if (__tls_memsz > __tls_filesz)
    memset(tp + __tls_filesz, 0, __tls_memsz - __tls_filesz);
  return tp;
}

// walk the auxiliary vector for the program headers, and those for the
// PT_TLS entry that describes the thread-local data.
static void
find_tls(uint64 *auxv)
{
  struct proghdr *phdr = 0;
  int phnum = 0;

  for (; auxv[0]; auxv += 2) {
    if (auxv[0] == AT_PHDR)
      phdr = (struct proghdr *)auxv[1];
    else if (auxv[0] == AT_PHNUM)
      phnum = auxv[1];
  }
  if (phdr == 0)
    return;

  // a position-independent program was loaded somewhere other than the
  // addresses in its headers; the difference is what PT_PHDR says
  // against where the headers really are.
  uint64 base = 0;
  for (int i = 0; i < phnum; i++)
    if (phdr[i].type == ELF_PROG_PHDR)
      base = (uint64)phdr - phdr[i].vaddr;

  for (int i = 0; i < phnum; i++) {
    if (phdr[i].type == ELF_PROG_TLS) {
      __tls_image = (void *)(base + phdr[i].vaddr);
      __tls_filesz = phdr[i].filesz;
      __tls_memsz = phdr[i].memsz;
      __tls_align = phdr[i].align;
      return;
    }
  }
}

// the ELF entry point calls this with the stack exec built.
//
// no canary of its own: this is where the canary is chosen, and a
// function checks on the way out the value as the global stands then
// (user/libc/canary.c).
__attribute__((no_stack_protector)) void
start(int argc, char **argv, char **envp)
{
  extern int main(int argc, char **argv, char **envp);
  extern void __libc_canary_init(const void *);
  uint64 *auxv;

  // the auxiliary vector sits above the environment
  for (auxv = (uint64 *)envp; *auxv; auxv++)
    ;
  auxv++;
  for (uint64 *a = auxv; a[0]; a += 2)
    if (a[0] == AT_RANDOM) {
      __libc_canary_init((const void *)a[1]);
      break;
    }
  find_tls(auxv);

  void *block = main_block;
  if (__tls_size() > sizeof(main_block)) {
    // a program with a lot of thread-local data needs a real mapping,
    // and this runs before anything else could have used memory.
    block = mmap(0, __tls_size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED)
      _exit(127);
  }
  void *tp = __tls_init(block);
  asm volatile("mv tp, %0" ::"r"(tp));
  ((struct __tcb *)((char *)tp - sizeof(struct __tcb)))->tid = gettid();

  environ = envp;
  if (argc > 0 && argv[0]) {
    program_invocation_name = argv[0];
    char *slash = strrchr(argv[0], '/');
    program_invocation_short_name = slash ? slash + 1 : argv[0];
  }

  // `--help' is answered from the manual, here, so that every program
  // answers it the same way and a program written tomorrow answers it
  // without being told to. A program with no manual page keeps whatever
  // it did before: __libc_help says whether it printed anything.
  if (argc == 2 && argv[1] && strcmp(argv[1], "--help") == 0) {
    extern int __libc_help(const char *name);
    if (__libc_help(program_invocation_short_name))
      exit(0);
  }

  // main() may be declared with two arguments or three; the extra one
  // is harmless either way on this ABI.
  exit(main(argc, argv, envp));
}
