// a small check of where thread-local storage ends up
#include "user/user.h"
#include "user/libc/libc.h"
#include <pthread.h>

static __thread int a = 77;
static __thread char b[16] = "hello";

static void *
worker(void *arg)
{
  char *tp;
  asm volatile("mv %0, tp" : "=r"(tp));
  printf("thread: tp %p a %d (&a %p, off %ld) b \"%s\" (&b %p)\n", tp, a, &a, (char *)&a - tp, b, b);
  return arg;
}

int
main(void)
{
  char *tp;
  asm volatile("mv %0, tp" : "=r"(tp));
  printf("main:   tp %p a %d (&a %p, off %ld) b \"%s\"\n", tp, a, &a, (char *)&a - tp, b);
  printf("image %p filesz %ld memsz %ld align %ld size %ld\n", __tls_image, (long)__tls_filesz,
         (long)__tls_memsz, (long)__tls_align, (long)__tls_size());
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_join(t, 0);
  exit(0);
}
