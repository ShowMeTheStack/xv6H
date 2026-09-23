#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "platform.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "mm.h"
#include "elf.h"
#include "page.h"
#include "vfs.h"
#include "posix.h"
#include "errno.h"

// map ELF permissions to area permissions.
static int
flags2prot(int flags)
{
  int prot = 0;
  if (flags & 0x1)
    prot |= VMA_EXEC;
  if (flags & 0x2)
    prot |= VMA_WRITE;
  if (flags & 0x4)
    prot |= VMA_READ;
  return prot;
}

//
// the implementation of the exec() system call: the ELF's segments
// become file-backed areas of a new address space (nothing is read until
// a page is touched), the stack is an area at the top, and the arguments
// go on it. the old address space is released only once the new one is
// complete.
//
int
kexec(char *path, char **argv)
{
  return kexecve(path, argv, 0);
}

// exec with an environment: the stack it builds is the System V one,
//   sp -> argc, argv[], 0, envp[], 0, auxv pairs, AT_NULL
// with the strings above it. a0, a1 and a2 also hold argc, argv and envp.
// the span of addresses an image's segments cover, so that a place can
// be found for it before anything is mapped.
static int
elf_span(struct inode *ip, struct elfhdr *elf, uint64 *lo, uint64 *hi)
{
  struct proghdr ph;
  int off, i;

  *lo = ~0UL;
  *hi = 0;
  for (i = 0, off = elf->phoff; i < elf->phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      return -1;
    if (ph.type != ELF_PROG_LOAD || ph.memsz == 0)
      continue;
    if (PGROUNDDOWN(ph.vaddr) < *lo)
      *lo = PGROUNDDOWN(ph.vaddr);
    if (PGROUNDUP(ph.vaddr + ph.memsz) > *hi)
      *hi = PGROUNDUP(ph.vaddr + ph.memsz);
  }
  return *lo <= *hi ? 0 : -1;
}

// load one ELF image's segments into the address space at `base`, as
// file-backed areas. returns the highest address used, or 0 on error,
// and reports where the program headers landed.
static uint64
load_elf(struct mm *mm, struct inode *ip, struct elfhdr *elf, uint64 base, uint64 *phdr_addr,
         char *interp, int interplen)
{
  struct proghdr ph;
  uint64 top = 0;
  int off;
  int i;

  *phdr_addr = 0;
  if (interp)
    interp[0] = 0;
  for (i = 0, off = elf->phoff; i < elf->phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      return 0;
    if (ph.type == ELF_PROG_INTERP) {
      if (interp == 0 || ph.filesz == 0 || ph.filesz >= (uint64)interplen)
        return 0;
      if (readi(ip, 0, (uint64)interp, ph.off, ph.filesz) != (int)ph.filesz)
        return 0;
      interp[ph.filesz] = 0;
      continue;
    }
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      return 0;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      return 0;
    if (ph.vaddr % PGSIZE != ph.off % PGSIZE)
      return 0; // the file offset and the address must agree modulo a page
    if (elf->phoff >= ph.off && elf->phoff < ph.off + ph.filesz && *phdr_addr == 0)
      *phdr_addr = base + ph.vaddr + (elf->phoff - ph.off);
    if (ph.memsz == 0)
      continue;

    uint64 start = PGROUNDDOWN(base + ph.vaddr);
    uint64 end = PGROUNDUP(base + ph.vaddr + ph.memsz);
    if (end > mm->stack_bottom - PGSIZE)
      return 0;
    if (vma_add(mm, start, end, flags2prot(ph.flags), VMA_FILE | VMA_FIXED, idup(ip),
                ph.off - (base + ph.vaddr - start), base + ph.vaddr + ph.filesz) < 0)
      return 0;
    if (end > top)
      top = end;
  }
  return top;
}

static int exec_depth(char *path, char **argv, char **envp, int depth);

int
kexecve(char *path, char **argv, char **envp)
{
  return exec_depth(path, argv, envp, 0);
}

static int
exec_depth(char *path, char **argv, char **envp, int depth)
{
  char *s, *last;
  uint64 argc, envc, sz = 0, sp, ustack[MAXARG];
  struct elfhdr elf, ielf;
  struct inode *ip, *iip = 0;
  struct mm *mm = 0, *oldmm;
  struct proc *p = myproc();
  uint64 phdr_addr = 0, base = 0, interp_base = 0, entry;
  char interp[MAXPATH];
  int err = -ENOEXEC; // what to report if the image cannot be run


  // Open the executable file.
  if ((ip = namei(path)) == 0) {
    return -ENOENT;
  }
  // a file system mounted noexec holds data, whatever the file looks
  // like. this is the check, and it is here rather than at open so
  // that a program on such a mount can still be read and copied.
  if (ip->sb && (ip->sb->flags & MS_NOEXEC)) {
    iput(ip);
    return -EACCES;
  }
  ilock(ip);

  // it has to be a file this process may run
  if (ip->type != T_FILE) {
    err = -EACCES;
    goto bad;
  }
  if ((err = inode_permitted(ip, 1)) < 0)
    goto bad;

  // set-user-id and set-group-id: the program runs as its owner. a
  // mount made nosuid, or a process that asked for no_new_privs, gets
  // nothing; and the ids it would have gone back to are replaced, so
  // the program cannot drop the privilege and take it up again.
  uint new_euid = myproc()->cred.euid, new_egid = myproc()->cred.egid;
  int setid = 0;
  if (!(ip->sb && (ip->sb->flags & MS_NOSUID)) && !myproc()->cred.no_new_privs) {
    if (ip->mode & 04000) {
      new_euid = ip->uid;
      setid = 1;
    }
    if (ip->mode & 02000) {
      new_egid = ip->gid;
      setid = 1;
    }
  }

  // Read the ELF header.
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // a file that starts with #! names the program that should read it,
  // with one optional argument, and the file itself follows as the
  // next argument. the interpreter may not itself be a script.
  if (((char *)&elf)[0] == '#' && ((char *)&elf)[1] == '!') {
    if (depth > 0) {
      err = -ELOOP; // a script whose interpreter is a script
      goto bad;
    }
    char line[MAXPATH + MAXPATH];
    int n = readi(ip, 0, (uint64)line, 0, sizeof(line) - 1);
    if (n <= 2)
      goto bad;
    line[n] = 0;
    for (int i = 0; i < n; i++)
      if (line[i] == '\n') {
        line[i] = 0;
        break;
      }
    char *prog = line + 2;
    while (*prog == ' ' || *prog == '\t')
      prog++;
    char *arg = prog;
    while (*arg && *arg != ' ' && *arg != '\t')
      arg++;
    if (*arg) {
      *arg++ = 0;
      while (*arg == ' ' || *arg == '\t')
        arg++;
      if (*arg == 0)
        arg = 0;
    } else {
      arg = 0;
    }
    if (*prog == 0)
      goto bad;

    // rebuild the argument list: the interpreter, its argument, the
    // script's path, and everything the caller passed after argv[0].
    char *newargv[MAXARG]; // on the stack: two processes may be here at once
    int k = 0;
    newargv[k++] = prog;
    if (arg)
      newargv[k++] = arg;
    newargv[k++] = path;
    for (int i = 1; argv[i] && k < MAXARG - 1; i++)
      newargv[k++] = argv[i];
    newargv[k] = 0;

    iunlockput(ip);
    ip = 0;
    return exec_depth(prog, newargv, envp, depth + 1);
  }

  // Is this really an ELF file?
  if (elf.magic != ELF_MAGIC)
    goto bad;

  if ((mm = proc_mm_create(p)) == 0) {
    err = -ENOMEM;
    goto bad;
  }

  // a position-independent executable is loaded away from address
  // zero, so that a null dereference has no page to land on.
  if (elf.type == ET_DYN)
    base = ELF_PIE_BASE;
  else if (elf.type != ET_EXEC)
    goto bad;

  // the segments, as areas backed by the file, and the interpreter's
  // name if the program asks for one.
  sz = load_elf(mm, ip, &elf, base, &phdr_addr, interp, sizeof(interp));
  if (sz == 0)
    goto bad;
  entry = base + elf.entry;

  // the interpreter, loaded above the program and entered instead of
  // it: ld.so relocates everything and calls the program itself.
  if (interp[0]) {
    iunlockput(ip);
    ip = 0;
    if ((iip = namei(interp)) == 0) {
      err = -ENOENT;
      goto bad;
    }
    ilock(iip);
    if (readi(iip, 0, (uint64)&ielf, 0, sizeof(ielf)) != sizeof(ielf))
      goto bad;
    if (ielf.magic != ELF_MAGIC || (ielf.type != ET_DYN && ielf.type != ET_EXEC))
      goto bad;
    // the interpreter goes just below the area mmap() hands out, far
    // from the heap, which grows up from the program's own end.
    uint64 ilo, ihi;
    if (ielf.type == ET_DYN) {
      if (elf_span(iip, &ielf, &ilo, &ihi) < 0)
        goto bad;
      interp_base = PGROUNDDOWN(mm->mmap_base - (ihi - ilo) - PGSIZE);
      mm->mmap_base = interp_base;
    } else {
      interp_base = 0;
    }
    uint64 idummy = 0;
    if (load_elf(mm, iip, &ielf, interp_base, &idummy, 0, 0) == 0)
      goto bad;
    entry = interp_base + ielf.entry;
    iunlockput(iip);
    iip = 0;
  }

  if (ip) {
    iunlockput(ip);
    ip = 0;
  }

  p = myproc();
  mm->heap_start = sz;
  mm->brk = sz;

  // the stack: an area below the trapframe, grown by faults, with the
  // page under it unmapped as a guard.
  if (mm_setup_stack(mm) < 0)
    goto bad;
  sp = mm->stack_top;
  uint64 stackbase = mm->stack_bottom;

  // the strings: the arguments, then the environment, bounded together
  // by ARG_MAX.
  uint64 argbytes = 0;
  static uint64 envstack[MAXENV];
  for (argc = 0; argv[argc]; argc++) {
    argbytes += strlen(argv[argc]) + 1;
    if (argc >= MAXARG || argbytes > ARG_MAX) {
      err = -E2BIG;
      goto bad;
    }
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if (sp < stackbase)
      goto bad;
    if (copyout(mm, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  for (envc = 0; envp && envp[envc]; envc++) {
    argbytes += strlen(envp[envc]) + 1;
    if (envc >= MAXENV - 1 || argbytes > ARG_MAX) {
      err = -E2BIG;
      goto bad;
    }
    sp -= strlen(envp[envc]) + 1;
    sp -= sp % 16;
    if (sp < stackbase)
      goto bad;
    if (copyout(mm, sp, envp[envc], strlen(envp[envc]) + 1) < 0)
      goto bad;
    envstack[envc] = sp;
  }
  envstack[envc] = 0;

  // sixteen random bytes for the runtime's stack guard (AT_RANDOM)
  char randbytes[16];
  random_bytes(randbytes, sizeof(randbytes));
  sp -= sizeof(randbytes);
  sp -= sp % 16;
  uint64 randaddr = sp;
  if (sp < stackbase || copyout(mm, sp, randbytes, sizeof(randbytes)) < 0)
    goto bad;

  // the auxiliary vector
  uint64 aux[24];
  int na = 0;
  aux[na++] = AT_PAGESZ;  aux[na++] = PGSIZE;
  aux[na++] = AT_ENTRY;   aux[na++] = base + elf.entry;
  aux[na++] = AT_PHDR;    aux[na++] = phdr_addr;
  aux[na++] = AT_PHENT;   aux[na++] = sizeof(struct proghdr);
  aux[na++] = AT_PHNUM;   aux[na++] = elf.phnum;
  aux[na++] = AT_BASE;    aux[na++] = interp_base;
  aux[na++] = AT_CLKTCK;  aux[na++] = tick_hz;
  aux[na++] = AT_RANDOM;  aux[na++] = randaddr;
  aux[na++] = AT_SECURE;  aux[na++] = 0;
  aux[na++] = AT_HWCAP;   aux[na++] = plat.misa;
  aux[na++] = AT_NULL;    aux[na++] = 0;

  // the block sp points at: argc, argv[], 0, envp[], 0, auxv
  uint64 nwords = 1 + (argc + 1) + (envc + 1) + na;
  sp -= nwords * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  {
    uint64 at = sp;
    if (copyout(mm, at, (char *)&argc, sizeof(argc)) < 0)
      goto bad;
    at += sizeof(uint64);
    p->trapframe->a1 = at;
    if (copyout(mm, at, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0)
      goto bad;
    at += (argc + 1) * sizeof(uint64);
    p->trapframe->a2 = at;
    if (copyout(mm, at, (char *)envstack, (envc + 1) * sizeof(uint64)) < 0)
      goto bad;
    at += (envc + 1) * sizeof(uint64);
    if (copyout(mm, at, (char *)aux, na * sizeof(uint64)) < 0)
      goto bad;
  }

  // past the last way to fail: this thread becomes the process, and
  // the other threads of the group are asked to leave.
  tgroup_take_over(p);
  tgroup_kill_others(p);

  sighand_exec(p);      // handled signals go back to the default
  fdtable_cloexec(p);   // and the close-on-exec descriptors are closed

  // who the new program is. a set-user-id program's saved id is the
  // one it took on, so it can drop the privilege and take it back; an
  // ordinary program's is its effective id, so it cannot. capabilities
  // follow the effective user id the same way they do for setuid(2).
  {
    struct cred *c = &p->cred;
    c->euid = new_euid;
    c->egid = new_egid;
    c->suid = c->euid;
    c->sgid = c->egid;
    if (c->euid == 0)
      c->cap_effective = c->cap_permitted = c->cap_bounding;
    else if (setid)
      c->cap_effective = c->cap_permitted = 0;
    else
      c->cap_effective = c->cap_permitted =
          (c->cap_permitted & c->cap_inheritable) & c->cap_bounding;
  }

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image. a thread's trapframe was mapped at an
  // address of its own in the old space; in the new one it goes back
  // to TRAPFRAME with everything else.
  oldmm = p->mm;
  if (p->tf_va != TRAPFRAME)
    mm_del_trapframe(oldmm, p);
  p->tf_va = TRAPFRAME;
  p->mm = mm;
  memset(p->trapframe->fregs, 0, sizeof(p->trapframe->fregs));
  p->trapframe->fcsr = 0;
  memset(p->trapframe->vcsrs, 0, sizeof(p->trapframe->vcsrs));
  if (p->vstate)
    memset(p->vstate, 0, PGSIZE);
  p->trapframe->epc = entry; // the interpreter's entry, or the program's
  p->trapframe->sp = sp;         // initial stack pointer
  proc_mm_free(oldmm);

  // a watched process stops once here, before the new program's first
  // instruction, so that whoever is watching sees what it became
  if (p->traced)
    ptrace_exec_stop(p);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

bad:
  if (mm)
    proc_mm_free(mm);
  if (iip)
    iunlockput(iip);
  if (ip)
    iunlockput(ip);
  return err;
}
