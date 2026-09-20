#ifndef _DLFCN_H
#define _DLFCN_H
#include <sys/cdefs.h>
#include <stddef.h>

#define RTLD_LAZY     0x0001
#define RTLD_NOW      0x0002
#define RTLD_NOLOAD   0x0004
#define RTLD_NODELETE 0x1000
#define RTLD_GLOBAL   0x0100
#define RTLD_LOCAL    0x0000

#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)

typedef struct {
  const char *dli_fname;  // the file the address came from
  void *dli_fbase;        // where that file was loaded
  const char *dli_sname;  // the symbol containing the address
  void *dli_saddr;        // where that symbol starts
} Dl_info;

struct dl_phdr_info {
  unsigned long dlpi_addr;
  const char *dlpi_name;
  const void *dlpi_phdr;
  unsigned short dlpi_phnum;
};

void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
int dladdr(const void *addr, Dl_info *info);
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *info, size_t size, void *data), void *data);
#endif
