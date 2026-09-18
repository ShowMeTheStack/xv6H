//
// vmm: manage xv6 guest virtual machines. every guest is two files in the
// host file system: a kernel (ELF at any load address, or a raw binary)
// and a disk image.
//
//   vmm start [-n NAME] [-k KERNEL] [-f DISK|none] [-c NCPU] [-m MB] [-r ADDR]
//             [-a "kernel command line"] [-H] [-s] [-d]
//     -H: secondary vcpus start stopped and are started through SBI HSM (Linux)
//   vmm ls                       list VMs
//   vmm attach VM                connect this terminal to VM's console (Ctrl-] detaches)
//   vmm kill VM | --all          halt and free
//   vmm restart VM               kill, then start again with the same settings
//   vmm pause VM | resume VM     stop / continue all of a VM's vcpus
//   vmm ps VM                    ask the guest for its process list (Ctrl-P)
//   vmm info VM                  detailed state of VM (vcpu 0 registers)
//   vmm stats                    hypervisor statistics
//
// VM is an id or a name. defaults: -k /kernel -f /disk0.img -c 1 -m 128.
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/vmm.h"
#include "user/user.h"

#define DETACH_CHAR 0x1d // Ctrl-]
#define MAXVM 16

static char *states[] = {"free", "new", "running", "halting", "paused"};
static char *intcs[] = {"plic", "aplic", "aplic-msi+imsic"};

static void
usage(void)
{
  fprintf(2, "usage: vmm start [-n name] [-k kernel] [-f disk|none] [-c ncpu] [-m mb] [-r addr]\n"
             "                 [-a bootargs] [-H] [-s] [-d]\n"
             "       vmm ls | attach VM | kill VM|--all | restart VM | pause VM | resume VM\n"
             "       vmm ps VM | info VM | stats\n");
  exit(1);
}

static int
getinfo(int id, struct vminfo *vi)
{
  return vm_info(id, vi);
}

// resolve an id or a name to a VM id; -1 if not found.
static int
lookup(char *s)
{
  struct vminfo vi;
  int digits = 1;
  for (char *p = s; *p; p++)
    if (*p < '0' || *p > '9')
      digits = 0;
  if (digits && *s) {
    int id = atoi(s);
    if (getinfo(id, &vi) == 0 && vi.state != VM_FREE)
      return id;
    return -1;
  }
  for (int id = 0; id < MAXVM && getinfo(id, &vi) == 0; id++)
    if (vi.state != VM_FREE && strcmp(vi.cfg.name, s) == 0)
      return id;
  return -1;
}

static int
must_lookup(char *s)
{
  int id = lookup(s);
  if (id < 0) {
    fprintf(2, "vmm: no vm '%s'\n", s);
    exit(1);
  }
  return id;
}

static uint64
parse_addr(char *s)
{
  uint64 v = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    s += 2;
  for (; *s; s++) {
    int d;
    if (*s >= '0' && *s <= '9')
      d = *s - '0';
    else if (*s >= 'a' && *s <= 'f')
      d = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F')
      d = *s - 'A' + 10;
    else {
      fprintf(2, "vmm: bad address\n");
      exit(1);
    }
    v = v * 16 + d;
  }
  return v;
}

// copy guest console output to our stdout until the VM goes away.
static void
output_loop(int id)
{
  char buf[128];
  int n;
  while ((n = vm_cons(id, VMCONS_READ, buf, sizeof(buf))) > 0)
    write(1, buf, n);
  exit(0);
}

static void
attach(int id)
{
  struct vminfo vi;
  if (getinfo(id, &vi) < 0 || vi.state == VM_FREE) {
    fprintf(2, "vmm: no vm %d\n", id);
    exit(1);
  }
  printf("[attached to vm %d%s%s, Ctrl-] to detach]\n", id, vi.cfg.name[0] ? " " : "", vi.cfg.name);
  consraw(1);
  int reader = fork();
  if (reader == 0)
    output_loop(id);
  char c;
  while (read(0, &c, 1) == 1) {
    if (c == DETACH_CHAR)
      break;
    if (vm_cons(id, VMCONS_WRITE, &c, 1) < 0)
      break;
    if (getinfo(id, &vi) < 0 || vi.state == VM_FREE)
      break;
  }
  vm_cons(id, VMCONS_DETACH, 0, 0);
  kill(reader);
  wait(0);
  consraw(0);
  printf("\n[detached from vm %d]\n", id);
}

// create and start a VM from cfg; returns its id.
static int
launch(struct vmconfig *cfg)
{
  int id = vm_create(cfg);
  if (id < 0) {
    fprintf(2, "vmm: cannot create vm (kernel/disk missing, disk in use, no memory, or no hypervisor?)\n");
    return -1;
  }
  for (int i = 0; i < cfg->nvcpu; i++) {
    int pid = fork();
    if (pid < 0) {
      fprintf(2, "vmm: fork failed\n");
      vm_destroy(id);
      return -1;
    }
    if (pid == 0) {
      vm_run(id, i);
      exit(0);
    }
  }
  printf("vm %d%s%s: %d vcpu(s), %d MB, kernel %s, disk %s\n", id, cfg->name[0] ? " " : "",
         cfg->name, cfg->nvcpu, cfg->mem_mb, cfg->kernel, cfg->disk[0] ? cfg->disk : "none");
  return id;
}

static void
setstr(char *dst, int max, char *src)
{
  int n = strlen(src);
  if (n >= max) {
    fprintf(2, "vmm: '%s' is too long\n", src);
    exit(1);
  }
  memmove(dst, src, n + 1);
}

static void
start(int argc, char *argv[])
{
  struct vmconfig cfg;
  int detach = 0;

  memset(&cfg, 0, sizeof(cfg));
  setstr(cfg.kernel, VM_PATHLEN, "/kernel");
  setstr(cfg.disk, VM_PATHLEN, "/disk0.img");
  cfg.nvcpu = 1;
  cfg.mem_mb = 128;

  for (int i = 0; i < argc; i++) {
    char *a = argv[i];
    char *v = (i + 1 < argc) ? argv[i + 1] : 0;
    if (strcmp(a, "-n") == 0 && v) {
      setstr(cfg.name, VM_NAMELEN, v);
      i++;
    } else if (strcmp(a, "-k") == 0 && v) {
      setstr(cfg.kernel, VM_PATHLEN, v);
      i++;
    } else if (strcmp(a, "-f") == 0 && v) {
      if (strcmp(v, "none") == 0)
        cfg.disk[0] = 0;
      else
        setstr(cfg.disk, VM_PATHLEN, v);
      i++;
    } else if (strcmp(a, "-c") == 0 && v) {
      cfg.nvcpu = atoi(v);
      i++;
    } else if (strcmp(a, "-m") == 0 && v) {
      cfg.mem_mb = atoi(v);
      i++;
    } else if (strcmp(a, "-r") == 0 && v) {
      cfg.flags |= VMF_RAW;
      cfg.load_addr = parse_addr(v);
      i++;
    } else if (strcmp(a, "-a") == 0 && v) {
      // the shell has no quoting: take every following word that is not
      // an option as part of the command line (quotes are stripped).
      int len = 0;
      while (i + 1 < argc && argv[i + 1][0] != '-') {
        char *w = argv[++i];
        int n = strlen(w);
        if (n && w[0] == '"') { w++; n--; }
        if (n && w[n - 1] == '"') n--;
        if (len + n + 2 > sizeof(cfg.bootargs)) {
          fprintf(2, "vmm: bootargs too long\n");
          exit(1);
        }
        if (len)
          cfg.bootargs[len++] = ' ';
        memmove(cfg.bootargs + len, w, n);
        len += n;
        cfg.bootargs[len] = 0;
      }
    } else if (strcmp(a, "-H") == 0) {
      cfg.flags |= VMF_HSM;
    } else if (strcmp(a, "-s") == 0) {
      cfg.flags |= VMF_SOFTIMSIC;
    } else if (strcmp(a, "-d") == 0) {
      detach = 1;
    } else {
      usage();
    }
  }
  if (cfg.nvcpu <= 0 || cfg.mem_mb <= 0)
    usage();
  if (cfg.name[0] && lookup(cfg.name) >= 0) {
    fprintf(2, "vmm: a vm named '%s' already exists\n", cfg.name);
    exit(1);
  }

  int id = launch(&cfg);
  if (id < 0)
    exit(1);
  if (!detach)
    attach(id);
  exit(0);
}

static void
pad(char *s, int width)
{
  printf("%s", s);
  for (int n = strlen(s); n < width; n++)
    printf(" ");
}

static void
list(void)
{
  struct vminfo vi;
  int any = 0;
  for (int id = 0; id < MAXVM && getinfo(id, &vi) == 0; id++) {
    if (vi.state == VM_FREE)
      continue;
    if (!any)
      printf("ID NAME             STATE    CPUS MEM  KERNEL             DISK               INTC\n");
    any = 1;
    printf("%d  ", id);
    pad(vi.cfg.name[0] ? vi.cfg.name : "-", 17);
    pad(states[vi.state % 5], 9);
    printf("%d    %d  ", vi.nvcpu, vi.mem_mb);
    pad(vi.cfg.kernel, 19);
    pad(vi.cfg.disk[0] ? vi.cfg.disk : "none", 19);
    printf("%s%s\n", intcs[vi.intc % 3], vi.imsic_hw ? " (hw)" : "");
  }
  if (!any)
    printf("no vms\n");
}

static void
info(int id)
{
  struct vminfo vi;
  if (getinfo(id, &vi) < 0)
    exit(1);
  printf("vm %d: name '%s' state %s, %d vcpu(s), %d MB, intc %s%s\n", id, vi.cfg.name,
         states[vi.state % 5], vi.nvcpu, vi.mem_mb, intcs[vi.intc % 3],
         vi.imsic_hw ? " (hardware guest files)" : "");
  printf("  kernel %s%s entry 0x%lx, disk %s (%ld sectors)%s\n", vi.cfg.kernel,
         (vi.cfg.flags & VMF_RAW) ? " (raw)" : "", vi.entry,
         vi.cfg.disk[0] ? vi.cfg.disk : "none", vi.disk_sectors,
         (vi.cfg.flags & VMF_HSM) ? ", hsm boot" : "");
  if (vi.cfg.bootargs[0])
    printf("  bootargs: %s\n", vi.cfg.bootargs);
  printf("  exits %ld: mmio %ld, csr %ld, wfi sleeps %ld, injected %ld\n", vi.exits, vi.mmio, vi.csr,
         vi.wfi, vi.injected);
  printf("  vcpu0: pc 0x%lx last cause 0x%lx gpa 0x%lx hart %d idle %d mmode %d\n", vi.pc,
         vi.last_cause, vi.last_gpa, vi.running_hart, vi.idle, vi.mmode);
  printf("         vsatp 0x%lx vstvec 0x%lx vsepc 0x%lx vscause 0x%lx vsstatus 0x%lx vstval 0x%lx\n",
         vi.vsatp, vi.vstvec, vi.vsepc, vi.vscause, vi.vsstatus, vi.vstval);
  printf("         sp 0x%lx ra 0x%lx vsie 0x%lx vstimecmp 0x%lx\n", vi.sp, vi.ra, vi.vsie,
         vi.vstimecmp);
}

static void
stats(void)
{
  uint64 s[2 + MAXVM];
  memset(s, 0, sizeof(s));
  if (vm_stats(s, 2 + MAXVM) < 0) {
    fprintf(2, "vmm: vm_stats failed\n");
    exit(1);
  }
  printf("free guest memory: %ld MB, ipis: %ld, exits:", s[0] * 2, s[1]);
  for (int i = 0; i < MAXVM; i++)
    if (s[2 + i])
      printf(" vm%d=%ld", i, s[2 + i]);
  printf("\n");
}

static void
wait_freed(int id)
{
  struct vminfo vi;
  for (int i = 0; i < 100; i++) {
    if (getinfo(id, &vi) < 0 || vi.state == VM_FREE)
      return;
    pause(1);
  }
  fprintf(2, "vmm: vm %d did not stop\n", id);
  exit(1);
}

static void
killvm(int id)
{
  if (vm_destroy(id) < 0) {
    fprintf(2, "vmm: no vm %d\n", id);
    exit(1);
  }
  wait_freed(id);
  printf("vm %d killed\n", id);
}

static void
killall(void)
{
  struct vminfo vi;
  int n = 0;
  for (int id = 0; id < MAXVM && getinfo(id, &vi) == 0; id++) {
    if (vi.state != VM_FREE && vm_destroy(id) == 0) {
      wait_freed(id);
      printf("vm %d killed\n", id);
      n++;
    }
  }
  if (n == 0)
    printf("no vms\n");
}

static void
restart(int id)
{
  struct vminfo vi;
  if (getinfo(id, &vi) < 0 || vi.state == VM_FREE)
    exit(1);
  struct vmconfig cfg = vi.cfg;
  killvm(id);
  int nid = launch(&cfg);
  if (nid < 0)
    exit(1);
  printf("vm %d restarted as vm %d (detached)\n", id, nid);
}

// ask the guest for its process list: send Ctrl-P and show what it says.
static void
ps(int id)
{
  char c = 0x10;
  int reader = fork();
  if (reader == 0)
    output_loop(id);
  vm_cons(id, VMCONS_WRITE, &c, 1);
  pause(5);
  kill(reader);
  wait(0);
  vm_cons(id, VMCONS_DETACH, 0, 0);
  printf("\n");
}

int
main(int argc, char *argv[])
{
  if (argc < 2)
    usage();
  char *cmd = argv[1];
  if (strcmp(cmd, "start") == 0) {
    start(argc - 2, argv + 2);
  } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "list") == 0) {
    list();
  } else if (strcmp(cmd, "stats") == 0) {
    stats();
  } else if (argc == 3 && strcmp(cmd, "kill") == 0 &&
             (strcmp(argv[2], "--all") == 0 || strcmp(argv[2], "-a") == 0)) {
    killall();
  } else if (argc == 3) {
    int id = must_lookup(argv[2]);
    if (strcmp(cmd, "attach") == 0)
      attach(id);
    else if (strcmp(cmd, "kill") == 0)
      killvm(id);
    else if (strcmp(cmd, "restart") == 0)
      restart(id);
    else if (strcmp(cmd, "pause") == 0)
      printf("vm %d %s\n", id, vm_ctl(id, VMCTL_PAUSE) == 0 ? "paused" : "not paused");
    else if (strcmp(cmd, "resume") == 0)
      printf("vm %d %s\n", id, vm_ctl(id, VMCTL_RESUME) == 0 ? "resumed" : "not resumed");
    else if (strcmp(cmd, "ps") == 0)
      ps(id);
    else if (strcmp(cmd, "info") == 0)
      info(id);
    else
      usage();
  } else {
    usage();
  }
  exit(0);
}
