//
// ld.so: the dynamic loader.
//
// the kernel starts this instead of the program when the program's
// PT_INTERP names it. by the time it hands control on, every shared
// object the program needs is mapped, every relocation is applied and
// every symbol resolved, so the program begins in a world where its
// calls into the library simply work.
//
// nothing here may call the C library: the library is what this file
// is in the middle of loading. the system calls, the string functions
// and the error messages are all local, and small on purpose.
//
#include <stdint.h>
#include <stddef.h>
#include <elf.h>
#include "kernel/syscall.h"
#include "kernel/fcntl.h"
#include "kernel/mman.h"
#include "kernel/errno.h"

#define PAGESIZE 4096
#define PAGEDOWN(x) ((x) & ~(uint64_t)(PAGESIZE - 1))
#define PAGEUP(x) (((x) + PAGESIZE - 1) & ~(uint64_t)(PAGESIZE - 1))
#define MAXDSO 64
#define RTLD_NOLOAD_FLAG 0x0004
#define MAXPATHLEN 128

// ---- system calls, without the library's stubs ----
//
// the stubs in usys.S write errno through the thread pointer, which
// does not exist yet, so the loader makes its own calls.

static inline long
syscall6(long n, long a, long b, long c, long d, long e, long f)
{
  register long a0 asm("a0") = a;
  register long a1 asm("a1") = b;
  register long a2 asm("a2") = c;
  register long a3 asm("a3") = d;
  register long a4 asm("a4") = e;
  register long a5 asm("a5") = f;
  register long a7 asm("a7") = n;

  asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
  return a0;
}

#define SYSCALL0(n) syscall6((n), 0, 0, 0, 0, 0, 0)
#define SYSCALL1(n, a) syscall6((n), (long)(a), 0, 0, 0, 0, 0)
#define SYSCALL2(n, a, b) syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define SYSCALL3(n, a, b, c) syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SYSCALL6(n, a, b, c, d, e, f)                                                              \
  syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

static int
dl_open(const char *path, int flags)
{
  return (int)SYSCALL2(SYS_open, path, flags);
}

static long
dl_read(int fd, void *buf, long n)
{
  return SYSCALL3(SYS_read, fd, buf, n);
}

static long
dl_pread(int fd, void *buf, long n, uint64_t off)
{
  return syscall6(SYS_pread, fd, (long)buf, n, (long)off, 0, 0);
}

static void
dl_close(int fd)
{
  SYSCALL1(SYS_close, fd);
}

static long
dl_write(int fd, const void *buf, long n)
{
  return SYSCALL3(SYS_write, fd, buf, n);
}

static void *
dl_mmap(void *addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
  return (void *)SYSCALL6(SYS_mmap, addr, len, prot, flags, fd, off);
}

static void __attribute__((noreturn))
dl_exit(int code)
{
  SYSCALL1(SYS_exit, code);
  for (;;)
    ;
}

// ---- the little that stands in for a library ----

static size_t
dl_strlen(const char *s)
{
  const char *p = s;

  while (*p)
    p++;
  return p - s;
}

static int
dl_strcmp(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

static void
dl_memcpy(void *d, const void *s, size_t n)
{
  char *dp = d;
  const char *sp = s;

  while (n--)
    *dp++ = *sp++;
}

static void
dl_memset(void *d, int c, size_t n)
{
  char *dp = d;

  while (n--)
    *dp++ = c;
}

static void
dl_puts(const char *s)
{
  dl_write(2, s, dl_strlen(s));
}

static void
dl_puthex(uint64_t v)
{
  static const char digits[] = "0123456789abcdef";
  char buf[19];
  int n = 0;

  buf[n++] = '0';
  buf[n++] = 'x';
  int started = 0;
  for (int i = 60; i >= 0; i -= 4) {
    int d = (v >> i) & 0xf;
    if (d || started || i == 0) {
      buf[n++] = digits[d];
      started = 1;
    }
  }
  dl_write(2, buf, n);
}

// what to say when there is no way to carry on
static void __attribute__((noreturn))
die(const char *what, const char *detail)
{
  dl_puts("ld.so: ");
  dl_puts(what);
  if (detail) {
    dl_puts(": ");
    dl_puts(detail);
  }
  dl_puts("\n");
  dl_exit(127);
}

// ---- the objects we have loaded ----

struct dso {
  uint64_t base;            // where its vaddrs were placed
  const char *name;
  Elf64_Dyn *dynamic;
  const char *strtab;
  Elf64_Sym *symtab;
  uint32_t *hash;           // DT_HASH: nbucket, nchain, buckets, chain
  Elf64_Rela *rela;
  size_t relasz, relaent;
  Elf64_Rela *jmprel;
  size_t pltrelsz;
  uint64_t *init_array;
  size_t init_arraysz;
  uint64_t init;
  Elf64_Phdr *phdr;
  int phnum;
  uint64_t tls_vaddr, tls_filesz, tls_memsz, tls_align, tls_offset;
  int has_tls;
  int relocated;
  char path[MAXPATHLEN];
};

static struct dso objects[MAXDSO];
static int nobjects;
// where a library is looked for: the proper place first, then the
// root, because the file system has no directories yet.
static const char *const libdirs[] = {"/lib/", "/", 0};


// what dladdr() fills in, and what dl_iterate_phdr() reports: the same
// shapes <dlfcn.h> declares, spelled out here because this file may
// not include the library's headers.
struct dl_info {
  const char *dli_fname;
  void *dli_fbase;
  const char *dli_sname;
  void *dli_saddr;
};

struct dl_phdr_info {
  uint64_t dlpi_addr;
  const char *dlpi_name;
  const void *dlpi_phdr;
  unsigned short dlpi_phnum;
};

// a table filled in for the library, so that dlopen and its relatives
// can reach the loader that holds the link map.
struct dl_vtable {
  void *(*open)(const char *path, int flags);
  void *(*sym)(void *handle, const char *name);
  int (*close)(void *handle);
  const char *(*error)(void);
  int (*addr)(const void *addr, struct dl_info *info);
  int (*iterate)(int (*cb)(struct dl_phdr_info *info, size_t size, void *data), void *data);
};

static const char *dl_last_error;

// ---- the first step: relocate ourselves ----
//
// this runs before any global has a usable address, so it may only
// touch what it is given. it applies the loader's own relative
// relocations, which is what makes everything after it ordinary C.

__attribute__((used)) void
__dls1(uint64_t base, Elf64_Dyn *dynamic)
{
  Elf64_Rela *rela = 0;
  size_t relasz = 0, relaent = sizeof(Elf64_Rela);

  for (Elf64_Dyn *d = dynamic; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_RELA:
      rela = (Elf64_Rela *)(base + d->d_un.d_ptr);
      break;
    case DT_RELASZ:
      relasz = d->d_un.d_val;
      break;
    case DT_RELAENT:
      relaent = d->d_un.d_val;
      break;
    default:
      break;
    }
  }
  for (size_t i = 0; rela && i < relasz / relaent; i++) {
    Elf64_Rela *r = (Elf64_Rela *)((char *)rela + i * relaent);
    if (ELF64_R_TYPE(r->r_info) == R_RISCV_RELATIVE)
      *(uint64_t *)(base + r->r_offset) = base + r->r_addend;
  }
}

// ---- symbols ----

static uint32_t
sysv_hash(const char *name)
{
  uint32_t h = 0, g;

  for (; *name; name++) {
    h = (h << 4) + (unsigned char)*name;
    if ((g = h & 0xf0000000))
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

// look a name up in one object, and report the symbol only if it is
// defined there.
static Elf64_Sym *
lookup_in(struct dso *o, const char *name, uint32_t h)
{
  if (o->hash == 0 || o->symtab == 0 || o->strtab == 0)
    return 0;

  uint32_t nbucket = o->hash[0];
  uint32_t *bucket = o->hash + 2;
  uint32_t *chain = bucket + nbucket;

  if (nbucket == 0)
    return 0;
  for (uint32_t i = bucket[h % nbucket]; i != 0; i = chain[i]) {
    Elf64_Sym *sym = &o->symtab[i];
    if (sym->st_shndx == SHN_UNDEF)
      continue;
    if (dl_strcmp(o->strtab + sym->st_name, name) != 0)
      continue;
    int bind = ELF64_ST_BIND(sym->st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK)
      continue;
    if (ELF64_ST_VISIBILITY(sym->st_other) == STV_HIDDEN ||
        ELF64_ST_VISIBILITY(sym->st_other) == STV_INTERNAL)
      continue;
    return sym;
  }
  return 0;
}

// the global search: every object in load order, the executable first.
static uint64_t
lookup(const char *name, struct dso **found, struct dso *skip)
{
  uint32_t h = sysv_hash(name);

  for (int i = 0; i < nobjects; i++) {
    if (&objects[i] == skip)
      continue;
    Elf64_Sym *sym = lookup_in(&objects[i], name, h);
    if (sym) {
      if (found)
        *found = &objects[i];
      if (ELF64_ST_TYPE(sym->st_info) == STT_TLS)
        return sym->st_value; // a TLS symbol's value is its module offset
      return objects[i].base + sym->st_value;
    }
  }
  if (found)
    *found = 0;
  return 0;
}

// ---- loading ----

static void
read_dynamic(struct dso *o)
{
  for (Elf64_Dyn *d = o->dynamic; d && d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_STRTAB: o->strtab = (const char *)(o->base + d->d_un.d_ptr); break;
    case DT_SYMTAB: o->symtab = (Elf64_Sym *)(o->base + d->d_un.d_ptr); break;
    case DT_HASH: o->hash = (uint32_t *)(o->base + d->d_un.d_ptr); break;
    case DT_RELA: o->rela = (Elf64_Rela *)(o->base + d->d_un.d_ptr); break;
    case DT_RELASZ: o->relasz = d->d_un.d_val; break;
    case DT_RELAENT: o->relaent = d->d_un.d_val; break;
    case DT_JMPREL: o->jmprel = (Elf64_Rela *)(o->base + d->d_un.d_ptr); break;
    case DT_PLTRELSZ: o->pltrelsz = d->d_un.d_val; break;
    case DT_INIT: o->init = o->base + d->d_un.d_ptr; break;
    case DT_INIT_ARRAY: o->init_array = (uint64_t *)(o->base + d->d_un.d_ptr); break;
    case DT_INIT_ARRAYSZ: o->init_arraysz = d->d_un.d_val; break;
    default: break;
    }
  }
  if (o->relaent == 0)
    o->relaent = sizeof(Elf64_Rela);
}

static struct dso *
find_loaded(const char *name)
{
  for (int i = 0; i < nobjects; i++)
    if (objects[i].name && dl_strcmp(objects[i].name, name) == 0)
      return &objects[i];
  return 0;
}

// map one shared object and fill in its entry. the whole span is
// reserved with one anonymous mapping so that the segments keep their
// relative addresses, and each segment is then mapped over it from the
// file, so that pages arrive as they are touched.
static struct dso *
load_object(const char *name)
{
  struct dso *o = find_loaded(name);

  if (o)
    return o;
  if (nobjects >= MAXDSO)
    die("too many shared objects", name);

  char path[MAXPATHLEN];
  int fd = -1;

  for (int d = 0; libdirs[d] && fd < 0; d++) {
    int n = 0;
    if (name[0] != '/')
      for (const char *dir = libdirs[d]; *dir && n < MAXPATHLEN - 2; dir++)
        path[n++] = *dir;
    for (const char *p = name; *p && n < MAXPATHLEN - 1; p++)
      path[n++] = *p;
    path[n] = 0;
    fd = dl_open(path, O_RDONLY);
    if (name[0] == '/')
      break;
  }
  if (fd < 0)
    die("cannot open", name);

  Elf64_Ehdr eh;
  if (dl_read(fd, &eh, sizeof(eh)) != sizeof(eh))
    die("cannot read", path);
  if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F')
    die("not an ELF file", path);
  if (eh.e_type != ET_DYN)
    die("not a shared object", path);

  Elf64_Phdr ph[16];
  if (eh.e_phnum > 16)
    die("too many program headers", path);
  if (dl_pread(fd, ph, (long)eh.e_phnum * sizeof(Elf64_Phdr), eh.e_phoff) !=
      (long)eh.e_phnum * sizeof(Elf64_Phdr))
    die("cannot read program headers", path);

  uint64_t lo = ~0UL, hi = 0;
  for (int i = 0; i < eh.e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD)
      continue;
    if (ph[i].p_vaddr < lo)
      lo = PAGEDOWN(ph[i].p_vaddr);
    if (ph[i].p_vaddr + ph[i].p_memsz > hi)
      hi = PAGEUP(ph[i].p_vaddr + ph[i].p_memsz);
  }
  if (lo > hi)
    die("no loadable segments", path);

  char *span = dl_mmap(0, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (span == MAP_FAILED)
    die("out of memory mapping", path);
  uint64_t base = (uint64_t)span - lo;

  for (int i = 0; i < eh.e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
      continue;
    uint64_t start = PAGEDOWN(base + ph[i].p_vaddr);
    uint64_t off = PAGEDOWN(ph[i].p_offset);
    uint64_t filelen = ph[i].p_filesz + (ph[i].p_vaddr - PAGEDOWN(ph[i].p_vaddr));
    int prot = 0;
    if (ph[i].p_flags & PF_R)
      prot |= PROT_READ;
    if (ph[i].p_flags & PF_W)
      prot |= PROT_WRITE;
    if (ph[i].p_flags & PF_X)
      prot |= PROT_EXEC;
    // relocations are applied into the text as well as the data, so
    // everything is mapped writable until the loader is done.
    if (filelen &&
        dl_mmap((void *)start, filelen, prot | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, off) ==
            MAP_FAILED)
      die("cannot map segment of", path);
    // the part of the segment with no file behind it is already zero,
    // because the span was mapped anonymously; but the tail of the
    // last file page needs clearing.
    uint64_t fileend = base + ph[i].p_vaddr + ph[i].p_filesz;
    uint64_t memend = base + ph[i].p_vaddr + ph[i].p_memsz;
    if (memend > fileend) {
      uint64_t tail = PAGEUP(fileend);
      if (tail > memend)
        tail = memend;
      dl_memset((void *)fileend, 0, tail - fileend);
      if (memend > tail)
        if (dl_mmap((void *)tail, memend - tail, prot | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
          die("cannot map bss of", path);
    }
  }
  dl_close(fd);

  o = &objects[nobjects++];
  dl_memset(o, 0, sizeof(*o));
  o->base = base;
  int i = 0;
  for (; path[i] && i < MAXPATHLEN - 1; i++)
    o->path[i] = path[i];
  o->path[i] = 0;
  o->name = o->path;
  for (int k = 0; o->path[k]; k++)
    if (o->path[k] == '/')
      o->name = o->path + k + 1;

  for (int k = 0; k < eh.e_phnum; k++) {
    if (ph[k].p_type == PT_DYNAMIC)
      o->dynamic = (Elf64_Dyn *)(base + ph[k].p_vaddr);
    else if (ph[k].p_type == PT_TLS) {
      o->has_tls = 1;
      o->tls_vaddr = base + ph[k].p_vaddr;
      o->tls_filesz = ph[k].p_filesz;
      o->tls_memsz = ph[k].p_memsz;
      o->tls_align = ph[k].p_align;
    }
  }
  if (o->dynamic == 0)
    die("no dynamic section in", path);
  read_dynamic(o);
  return o;
}

// every DT_NEEDED of every object, breadth first, which is the order
// symbol lookup must follow as well.
static void
load_needed(struct dso *o)
{
  for (Elf64_Dyn *d = o->dynamic; d->d_tag != DT_NULL; d++)
    if (d->d_tag == DT_NEEDED)
      load_object(o->strtab + d->d_un.d_val);
}

// ---- relocation ----

static void
apply(struct dso *o, Elf64_Rela *rela, size_t count)
{
  for (size_t i = 0; i < count; i++) {
    Elf64_Rela *r = &rela[i];
    uint64_t *where = (uint64_t *)(o->base + r->r_offset);
    uint32_t type = ELF64_R_TYPE(r->r_info);
    uint32_t symi = ELF64_R_SYM(r->r_info);
    const char *name = 0;
    uint64_t value = 0;
    struct dso *from = 0;

    if (symi) {
      Elf64_Sym *sym = &o->symtab[symi];
      name = o->strtab + sym->st_name;
      if (sym->st_shndx != SHN_UNDEF && ELF64_ST_BIND(sym->st_info) == STB_LOCAL) {
        value = o->base + sym->st_value;
        from = o;
      } else {
        // a copy relocation asks for the definition somewhere else:
        // the executable is where the copy is going, so looking there
        // would find the empty copy and copy it onto itself.
        value = lookup(name, &from, type == R_RISCV_COPY ? o : 0);
        if (from == 0) {
          if (ELF64_ST_BIND(sym->st_info) == STB_WEAK)
            value = 0;
          else
            die("undefined symbol", name);
        }
      }
    }

    switch (type) {
    case R_RISCV_NONE:
      break;
    case R_RISCV_RELATIVE:
      *where = o->base + r->r_addend;
      break;
    case R_RISCV_64:
      *where = value + r->r_addend;
      break;
    case R_RISCV_JUMP_SLOT:
      *where = value;
      break;
    case R_RISCV_COPY:
      // the executable keeps its own copy of a library's variable
      if (from && name)
        dl_memcpy(where, (void *)value, o->symtab[symi].st_size);
      break;
    case R_RISCV_TLS_TPREL64:
      // the offset of the variable from the thread pointer: the
      // module's own offset in the static block, plus the symbol's.
      *where = (from ? from->tls_offset : 0) + value + r->r_addend;
      break;
    case R_RISCV_TLS_DTPMOD64:
      *where = 1; // one module holds all the static TLS for now
      break;
    case R_RISCV_TLS_DTPREL64:
      *where = (from ? from->tls_offset : 0) + value + r->r_addend;
      break;
    default:
      dl_puts("ld.so: unsupported relocation type ");
      dl_puthex(type);
      dl_puts(" in ");
      dl_puts(o->name ? o->name : "?");
      dl_puts("\n");
      dl_exit(127);
    }
  }
}

static void
relocate(struct dso *o)
{
  if (o->relocated)
    return;
  o->relocated = 1;
  if (o->rela)
    apply(o, o->rela, o->relasz / o->relaent);
  if (o->jmprel)
    apply(o, o->jmprel, o->pltrelsz / sizeof(Elf64_Rela));
}

// ---- what the library calls through ----

static void
run_init(struct dso *o)
{
  if (o->init)
    ((void (*)(void))o->init)();
  for (size_t k = 0; o->init_array && k < o->init_arraysz / sizeof(uint64_t); k++)
    if (o->init_array[k] && o->init_array[k] != (uint64_t)-1)
      ((void (*)(void))(o->init_array[k]))();
}

static void *
dl_dlopen(const char *path, int flags)
{
  int before = nobjects;

  dl_last_error = 0;
  if (path == 0)
    return &objects[0]; // the program itself
  struct dso *o = find_loaded(path);
  if (o)
    return o;
  if (flags & RTLD_NOLOAD_FLAG)
    return 0;

  o = load_object(path);
  for (int i = before; i < nobjects; i++)
    load_needed(&objects[i]);
  for (int i = nobjects - 1; i >= before; i--)
    relocate(&objects[i]);
  for (int i = nobjects - 1; i >= before; i--)
    run_init(&objects[i]);
  return o;
}

static void *
dl_dlsym(void *handle, const char *name)
{
  dl_last_error = 0;
  if (handle == 0 || handle == (void *)-1) {
    struct dso *from = 0;
    uint64_t v = lookup(name, &from, 0);
    if (from == 0) {
      dl_last_error = "symbol not found";
      return 0;
    }
    return (void *)v;
  }
  struct dso *o = handle;
  Elf64_Sym *sym = lookup_in(o, name, sysv_hash(name));
  if (sym == 0) {
    // the dependencies of the object count as part of it
    for (Elf64_Dyn *d = o->dynamic; d->d_tag != DT_NULL && sym == 0; d++)
      if (d->d_tag == DT_NEEDED) {
        struct dso *dep = find_loaded(o->strtab + d->d_un.d_val);
        if (dep) {
          sym = lookup_in(dep, name, sysv_hash(name));
          if (sym)
            return (void *)(dep->base + sym->st_value);
        }
      }
    if (sym == 0) {
      dl_last_error = "symbol not found";
      return 0;
    }
  }
  return (void *)(o->base + sym->st_value);
}

static int
dl_dlclose(void *handle)
{
  // objects stay loaded: nothing here keeps a reference count, and
  // unmapping a library whose code may still be on a stack is worse
  // than holding the memory.
  (void)handle;
  return 0;
}

static const char *
dl_dlerror(void)
{
  const char *e = dl_last_error;

  dl_last_error = 0;
  return e;
}

static int
dl_dladdr(const void *addr, struct dl_info *info)
{
  uint64_t a = (uint64_t)addr;
  struct dso *best = 0;

  for (int i = 0; i < nobjects; i++)
    if (objects[i].base <= a && (best == 0 || objects[i].base > best->base))
      best = &objects[i];
  if (best == 0)
    return 0;

  info->dli_fname = best->name;
  info->dli_fbase = (void *)best->base;
  info->dli_sname = 0;
  info->dli_saddr = 0;

  // the symbol that starts closest below the address
  if (best->hash && best->symtab) {
    uint32_t nchain = best->hash[1];
    uint64_t bestval = 0;
    for (uint32_t i = 1; i < nchain; i++) {
      Elf64_Sym *sym = &best->symtab[i];
      if (sym->st_shndx == SHN_UNDEF || sym->st_value == 0)
        continue;
      uint64_t v = best->base + sym->st_value;
      if (v <= a && v > bestval) {
        bestval = v;
        info->dli_sname = best->strtab + sym->st_name;
        info->dli_saddr = (void *)v;
      }
    }
  }
  return 1;
}

static int
dl_iterate(int (*cb)(struct dl_phdr_info *info, size_t size, void *data), void *data)
{
  for (int i = 0; i < nobjects; i++) {
    struct dl_phdr_info info;
    info.dlpi_addr = objects[i].base;
    info.dlpi_name = objects[i].name;
    info.dlpi_phdr = objects[i].phdr;
    info.dlpi_phnum = objects[i].phnum;
    int r = cb(&info, sizeof(info), data);
    if (r)
      return r;
  }
  return 0;
}

// hand the library the functions above, if it is loaded and asks for
// them by defining the table.
static void
publish(void)
{
  struct dso *from = 0;
  uint64_t addr = lookup("__dl_vtable", &from, 0);

  if (from == 0 || addr == 0)
    return;
  struct dl_vtable *v = (struct dl_vtable *)addr;
  v->open = dl_dlopen;
  v->sym = dl_dlsym;
  v->close = dl_dlclose;
  v->error = dl_dlerror;
  v->addr = dl_dladdr;
  v->iterate = dl_iterate;
}

// ---- the second step: load the program's world ----

__attribute__((used)) uint64_t
__dls2(int argc, char **argv, char **envp, uint64_t ldbase)
{
  Elf64_Phdr *phdr = 0;
  int phnum = 0;
  uint64_t entry = 0;

  (void)argc;
  (void)argv;
  (void)ldbase;

  uint64_t *auxv = (uint64_t *)envp;
  while (*auxv)
    auxv++;
  auxv++;
  for (uint64_t *a = auxv; a[0]; a += 2) {
    if (a[0] == 3) // AT_PHDR
      phdr = (Elf64_Phdr *)a[1];
    else if (a[0] == 5) // AT_PHNUM
      phnum = (int)a[1];
    else if (a[0] == 9) // AT_ENTRY
      entry = a[1];
  }
  if (phdr == 0 || entry == 0)
    die("no program headers", 0);

  // the executable's own entry in the link map, built from what the
  // kernel already told us rather than by reading the file again.
  struct dso *exe = &objects[nobjects++];
  dl_memset(exe, 0, sizeof(*exe));
  exe->name = argv[0] ? argv[0] : "the program";
  exe->phdr = phdr;
  exe->phnum = phnum;

  uint64_t vaddr_of_phdr = 0;
  for (int i = 0; i < phnum; i++)
    if (phdr[i].p_type == PT_PHDR)
      vaddr_of_phdr = phdr[i].p_vaddr;
  exe->base = (uint64_t)phdr - vaddr_of_phdr;

  for (int i = 0; i < phnum; i++) {
    if (phdr[i].p_type == PT_DYNAMIC)
      exe->dynamic = (Elf64_Dyn *)(exe->base + phdr[i].p_vaddr);
    else if (phdr[i].p_type == PT_TLS) {
      exe->has_tls = 1;
      exe->tls_vaddr = exe->base + phdr[i].p_vaddr;
      exe->tls_filesz = phdr[i].p_filesz;
      exe->tls_memsz = phdr[i].p_memsz;
      exe->tls_align = phdr[i].p_align;
    }
  }
  if (exe->dynamic == 0)
    die("the program has no dynamic section", 0);
  read_dynamic(exe);

  // everything it needs, and everything those need in turn
  load_needed(exe);
  for (int i = 1; i < nobjects; i++)
    load_needed(&objects[i]);

  // the static TLS layout: the executable's block comes first, at
  // offset zero from the thread pointer, and the libraries follow.
  uint64_t tlsoff = 0;
  for (int i = 0; i < nobjects; i++) {
    struct dso *o = &objects[i];
    if (!o->has_tls)
      continue;
    uint64_t align = o->tls_align > 16 ? o->tls_align : 16;
    tlsoff = (tlsoff + align - 1) & ~(align - 1);
    o->tls_offset = tlsoff;
    tlsoff += o->tls_memsz;
  }

  // the libraries first, so that a symbol the executable copies is in
  // place before its copy relocation runs.
  for (int i = nobjects - 1; i >= 0; i--)
    relocate(&objects[i]);

  publish(); // so that dlopen and its relatives work

  // the initializers, libraries before the program
  for (int i = nobjects - 1; i >= 0; i--)
    run_init(&objects[i]);

  return entry;
}
