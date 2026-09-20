//
// <dlfcn.h>: loading a library while the program runs.
//
// the work belongs to the dynamic loader, which holds the link map;
// this file is the doorway. ld.so fills the table below in as it
// starts the program, so a dynamically linked program finds it ready
// and a statically linked one finds it empty and is told so.
//
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include "libc.h"

struct dl_vtable {
  void *(*open)(const char *path, int flags);
  void *(*sym)(void *handle, const char *name);
  int (*close)(void *handle);
  const char *(*error)(void);
  int (*addr)(const void *addr, Dl_info *info);
  int (*iterate)(int (*cb)(struct dl_phdr_info *info, size_t size, void *data), void *data);
};

// the loader looks this name up and writes the pointers into it
struct dl_vtable __dl_vtable;

static const char *
no_loader(void)
{
  return "this program is statically linked: there is no dynamic loader";
}

void *
dlopen(const char *path, int flags)
{
  if (__dl_vtable.open == 0)
    return 0;
  return __dl_vtable.open(path, flags);
}

void *
dlsym(void *handle, const char *name)
{
  if (__dl_vtable.sym == 0)
    return 0;
  return __dl_vtable.sym(handle, name);
}

int
dlclose(void *handle)
{
  if (__dl_vtable.close == 0)
    return -1;
  return __dl_vtable.close(handle);
}

char *
dlerror(void)
{
  if (__dl_vtable.error == 0)
    return (char *)no_loader();
  return (char *)__dl_vtable.error();
}

int
dladdr(const void *addr, Dl_info *info)
{
  if (__dl_vtable.addr == 0)
    return 0;
  return __dl_vtable.addr(addr, info);
}

int
dl_iterate_phdr(int (*cb)(struct dl_phdr_info *info, size_t size, void *data), void *data)
{
  if (__dl_vtable.iterate == 0)
    return 0;
  return __dl_vtable.iterate(cb, data);
}
