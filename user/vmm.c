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
#include "kernel/net.h"
#include "kernel/sysfilter.h"
#include "user/user.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "kernel/thread.h"
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>

#define DETACH_CHAR 0x1d // Ctrl-]
#define MAXVM 64

static const char *const states[] = {"free", "new", "running", "halting", "paused"};
static int lookup(char *s);
static int getinfo(int id, struct vminfo *vi);

static const char *const intcs[] = {"plic", "aplic", "aplic-msi+imsic"};

static void
usage(void)
{
  fprintf(stderr, "usage: vmm start [-n name] [-k kernel] [-f disk|none] [-c ncpu] [-m mb] [-r addr]\n"
             "                 [-a bootargs] [-i dhcp|IP/PREFIX[,GW[,DNS]]] [-H] [-s] [-4] [-Q] [-M mb] [-d]\n"
             "                 [-w weight] [--cpu-max pct] [-p HPORT[:GPORT][/udp]]\n"
             "       vmm ls | attach VM | kill VM|--all | restart VM | pause VM | resume VM\n"
             "       vmm ps VM | info VM | stats | balloon VM MB | ksm on|off\n"
             "       vmm weight VM N | vmm cpumax VM PCT\n"
             "       vmm port VM [ls] | vmm port VM add|rm SPEC...\n"
             "       vmm gw [ls] | vmm gw add NAME ADDR/PREFIX [pub IP] | vmm gw rm NAME\n"
             "       vmm save VM FILE | vmm snapshot VM FILE (live)\n"
             "       vmm restore FILE [-n NAME] [-d]\n"
             "       vmm clone BASE NEW [-f DISK] [-d]\n"
             "       vmm migrate VM HOST:PORT | vmm accept PORT [-n NAME] [-d]\n"
             "       vmm run [-n NAME] --rootfs DIR [-h HOST] [--mem PAGES]\n"
             "               [--pids N] [-u HOSTUID] [-d] [-i IP[/PREFIX]] [--gw NAME]\n"
             "               [--if NAME] [-p HPORT[:CPORT][/udp]] [--net host|none]\n"
             "               [-v HOST:PATH[:ro]]... [-e VAR=VALUE]... [CMD ARGS...]\n"
             "       vmm ps | vmm stop NAME | vmm exec NAME CMD...  the containers\n"
             "       (a machine behind a gateway: --nic IP/PREFIX,gw=GATEWAY [--gw NAME])\n");
  exit(1);
}

// "80", "8080:80", "53/udp", "8053:53/udp": the port outside, the port
// inside, and the protocol. Both ports are the same number when only
// one is given, which is what publishing a port usually means.
static int
parse_port(const char *a, struct pport *pp)
{
  char buf[32];
  int n = strlen(a);

  if (n == 0 || n >= (int)sizeof(buf))
    return -1;
  strcpy(buf, a);
  pp->proto = IPPROTO_TCP;
  char *slash = strchr(buf, '/');
  if (slash) {
    *slash = 0;
    if (strcmp(slash + 1, "udp") == 0)
      pp->proto = IPPROTO_UDP;
    else if (strcmp(slash + 1, "tcp") != 0)
      return -1;
  }
  char *colon = strchr(buf, ':');
  if (colon)
    *colon = 0;
  int h = atoi(buf);
  int gp = colon ? atoi(colon + 1) : h;
  if (h <= 0 || h > 65535 || gp <= 0 || gp > 65535)
    return -1;
  pp->hport = (uint16)h;
  pp->gport = (uint16)gp;
  return 0;
}

static const char *
protoname(uint8 proto)
{
  return proto == IPPROTO_UDP ? "udp" : "tcp";
}

// vmm port VM            -- what it publishes
// vmm port VM add SPEC   -- publish one more
// vmm port VM rm SPEC    -- stop publishing one
static int
cmd_port(int argc, char *argv[])
{
  if (argc < 1)
    usage();
  int id = lookup(argv[0]);
  if (id < 0) {
    fprintf(stderr, "vmm: no vm '%s'\n", argv[0]);
    return 1;
  }
  if (argc == 1 || strcmp(argv[1], "ls") == 0) {
    struct vminfo vi;
    if (getinfo(id, &vi) < 0 || vi.state == VM_FREE) {
      fprintf(stderr, "vmm: no vm '%s'\n", argv[0]);
      return 1;
    }
    if (vi.nports == 0) {
      printf("vm %d publishes nothing\n", id);
    } else {
      printf("HOST  GUEST  PROTO\n");
      for (int i = 0; i < vi.nports; i++)
        printf("%-5d %-6d %s\n", vi.ports[i].hport, vi.ports[i].gport,
               protoname(vi.ports[i].proto));
    }
    if (vi.net_filtered)
      printf("%ld frame(s) turned away\n", vi.net_filtered);
    return 0;
  }
  int add = strcmp(argv[1], "add") == 0;
  if (!add && strcmp(argv[1], "rm") != 0 && strcmp(argv[1], "del") != 0)
    usage();
  if (argc < 3)
    usage();
  for (int i = 2; i < argc; i++) {
    struct pport pp;
    if (parse_port(argv[i], &pp) < 0) {
      fprintf(stderr, "vmm: %s is not a port\n", argv[i]);
      return 1;
    }
    int rc = vm_ctl(id, add ? VMCTL_PORT_ADD : VMCTL_PORT_RM,
                    VMCTL_PORT(pp.proto, pp.hport, pp.gport));
    if (rc < 0) {
      fprintf(stderr, "vmm: %s %s: %s\n", add ? "add" : "rm", argv[i],
              strerror(errno));
      return 1;
    }
  }
  return 0;
}

// vmm gw add NAME ADDR/PREFIX [pub IP]   -- a router inside the host
// vmm gw rm NAME
// vmm gw [ls]
static int
cmd_gw(int argc, char *argv[])
{
  if (argc == 0 || strcmp(argv[0], "ls") == 0) {
    FILE *f = fopen("/proc/net/gateways", "r");
    if (f == 0) {
      fprintf(stderr, "vmm: /proc/net/gateways: %s\n", strerror(errno));
      return 1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
      printf("%s", line);
    fclose(f);
    return 0;
  }
  int add = strcmp(argv[0], "add") == 0;
  if (!add && strcmp(argv[0], "rm") != 0 && strcmp(argv[0], "del") != 0)
    usage();
  if (argc < 2)
    usage();

  struct gwreq r;
  memset(&r, 0, sizeof(r));
  strncpy(r.name, argv[1], sizeof(r.name) - 1);
  if (!add) {
    if (netgw(GW_DEL, &r) < 0) {
      fprintf(stderr, "vmm: gw rm %s: %s\n", r.name, strerror(errno));
      return 1;
    }
    printf("gateway %s removed\n", r.name);
    return 0;
  }
  if (argc < 3)
    usage();
  // ADDR/PREFIX: the address the gateway owns, on the subnet behind it
  char buf[32];
  strncpy(buf, argv[2], sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char *slash = strchr(buf, '/');
  int bits = 24;
  if (slash) {
    *slash = 0;
    bits = atoi(slash + 1);
  }
  struct in_addr in;
  if (inet_aton(buf, &in) == 0 || bits < 1 || bits > 30) {
    fprintf(stderr, "vmm: %s is not an address and a prefix\n", argv[2]);
    return 1;
  }
  r.addr = ntohl(in.s_addr);
  r.mask = ~((uint32)0) << (32 - bits);
  for (int i = 3; i < argc; i++) {
    if ((strcmp(argv[i], "pub") == 0 || strcmp(argv[i], "--pub") == 0) &&
        i + 1 < argc) {
      if (inet_aton(argv[++i], &in) == 0) {
        fprintf(stderr, "vmm: %s is not an address\n", argv[i]);
        return 1;
      }
      r.pub_ip = ntohl(in.s_addr);
    } else {
      usage();
    }
  }
  if (netgw(GW_ADD, &r) < 0) {
    fprintf(stderr, "vmm: gw add %s: %s\n", r.name, strerror(errno));
    return 1;
  }
  in.s_addr = htonl(r.addr);
  printf("gateway %s: %s/%d inside", r.name, inet_ntoa(in), bits);
  in.s_addr = htonl(r.pub_ip);
  printf(", %s outside\n", inet_ntoa(in));
  return 0;
}

// ---- containers ----
//
// A container is not a machine: it is a process tree of this machine's
// own, in namespaces of its own, with a control group around it. `vmm
// run' builds one out of the parts the kernel provides:
//
//   a control group      what it may take (pids.max, memory.max)
//   a user namespace     root inside it is an ordinary user outside
//   a mount namespace    what it sees mounted, and what "/" is
//   a pid namespace      its first process is number 1
//   a uts namespace      its own name
//
// and then, inside: the root it was given, /proc, /dev and /tmp of its
// own, and the command. What it does with all of that is its own
// business, and none of it is the machine's.
//
// The dance with the parent is because two of those cannot be done by
// the process itself: its uid map has to be written by somebody outside
// it (or it could widen its own), and a new pid namespace holds the
// *next* child rather than the caller. So: the child unshares and waits,
// the parent writes the map, the child becomes root inside and forks,
// and the grandchild is process 1 of the container.

static int
ct_put(const char *path, const char *value)
{
  int fd = open(path, O_WRONLY);

  if (fd < 0)
    return -1;
  int n = write(fd, value, strlen(value));
  close(fd);
  return n > 0 ? 0 : -1;
}

// the container's network. Without an address it is a namespace with a
// loopback and nothing else -- which is what a container should have
// unless somebody says otherwise, since the machine's network is not
// something a hostile process should be able to reach. With one, it is
// an interface on a gateway's private subnet: it reaches the world
// through the gateway's address, and the world reaches it only at the
// ports published here.
static int
ct_network(uint32 ip, const char *gwname, const char *ifname,
           struct pport *ports, int nports)
{
  struct gwreq r;

  if (unshare(CLONE_NEWNET) < 0) {
    fprintf(stderr, "vmm: unshare(net): %s\n", strerror(errno));
    return -1;
  }
  if (ip == 0) {
    if (nports)
      fprintf(stderr, "vmm: -p needs an address (-i)\n");
    return nports ? -1 : 0;
  }
  memset(&r, 0, sizeof(r));
  r.addr = ip;
  strncpy(r.name, gwname, sizeof(r.name) - 1);
  strncpy(r.ifname, ifname, sizeof(r.ifname) - 1);
  if (netgw(GW_ATTACH, &r) < 0) {
    fprintf(stderr, "vmm: attaching to the gateway: %s\n", strerror(errno));
    return -1;
  }
  for (int i = 0; i < nports; i++) {
    memset(&r, 0, sizeof(r));
    r.addr = ip;
    strncpy(r.name, gwname, sizeof(r.name) - 1);
    r.proto = ports[i].proto;
    r.pport = ports[i].hport;
    r.cport = ports[i].gport;
    if (netgw(GW_PUB, &r) < 0) {
      fprintf(stderr, "vmm: publishing %d: %s\n", ports[i].hport,
              strerror(errno));
      return -1;
    }
  }
  return 0;
}

static int
ct_cgroup(const char *name, const char *mem, const char *pids)
{
  char path[128], num[32];

  snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/procs", name);
  int fd = open(path, O_WRONLY);
  if (fd >= 0)
    close(fd);
  else if (ct_put("/sys/fs/cgroup/cgroup.create", name) < 0)
    return -1;
  if (mem) {
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/memory.max", name);
    if (ct_put(path, mem) < 0)
      return -1;
  }
  if (pids) {
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/pids.max", name);
    if (ct_put(path, pids) < 0)
      return -1;
  }
  snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/procs", name);
  snprintf(num, sizeof(num), "%d", getpid());
  return ct_put(path, num);
}

// the whole of what happens inside, once the namespaces are there
static void
ct_inside(const char *rootfs, const char *host, char **cmd,
          const char **vols, int nvols)
{
  // the root it was given has to be a mount before it can be pivoted
  // into: a directory bound onto itself is one.
  if (mount(rootfs, rootfs, "none", MS_BIND, "") < 0) {
    fprintf(stderr, "vmm: binding %s: %s\n", rootfs, strerror(errno));
    exit(1);
  }
  // the volumes, while the machine's own directories still have names:
  // HOST:PATH[:ro] binds a directory of the machine's at PATH inside.
  for (int i = 0; i < nvols; i++) {
    char spec[160], target[192];
    strncpy(spec, vols[i], sizeof(spec) - 1);
    spec[sizeof(spec) - 1] = 0;
    char *colon = strchr(spec, ':');
    if (colon == 0) {
      fprintf(stderr, "vmm: -v wants HOST:PATH[:ro]\n");
      exit(1);
    }
    *colon = 0;
    char *path = colon + 1, *ro = strchr(path, ':');
    int flags = MS_BIND;
    if (ro) {
      *ro = 0;
      if (strcmp(ro + 1, "ro") != 0) {
        fprintf(stderr, "vmm: -v: %s?\n", ro + 1);
        exit(1);
      }
      flags |= MS_RDONLY;
    }
    snprintf(target, sizeof(target), "%s%s", rootfs, path);
    mkdir(target, 0755);
    if (mount(spec, target, "none", flags, "") < 0) {
      fprintf(stderr, "vmm: -v %s: %s\n", vols[i], strerror(errno));
      exit(1);
    }
  }
  if (pivot_root(rootfs, rootfs) < 0) {
    fprintf(stderr, "vmm: pivot_root %s: %s\n", rootfs, strerror(errno));
    exit(1);
  }
  // and now "/" is the container's. What it needs mounted, it mounts;
  // none of it is mounted on the machine.
  mkdir("/proc", 0555);
  mkdir("/dev", 0555);
  mkdir("/tmp", 01777);
  if (mount("proc", "/proc", "procfs", 0, "") < 0)
    fprintf(stderr, "vmm: /proc: %s\n", strerror(errno));
  if (mount("dev", "/dev", "devfs", 0, "") < 0)
    fprintf(stderr, "vmm: /dev: %s\n", strerror(errno));
  if (mount("tmpfs", "/tmp", "tmpfs", 0, "") < 0)
    fprintf(stderr, "vmm: /tmp: %s\n", strerror(errno));
  if (host && sethostname(host, strlen(host)) < 0)
    fprintf(stderr, "vmm: hostname: %s\n", strerror(errno));
  chdir("/");
  // and now the door closes behind it: exec may not gain anything, and
  // the calls a container has no business making are gone. Both are
  // one-way, which is what makes what comes next safe to run.
  if (sysfilter(SYSFILTER_NO_NEW_PRIVS, 0) < 0)
    fprintf(stderr, "vmm: no_new_privs: %s\n", strerror(errno));
  if (sysfilter(SYSFILTER_CONTAINER, 0) < 0)
    fprintf(stderr, "vmm: sysfilter: %s\n", strerror(errno));
  execvp(cmd[0], cmd);
  fprintf(stderr, "vmm: %s: %s\n", cmd[0], strerror(errno));
  exit(1);
}

// vmm run [-n NAME] --rootfs DIR [-h HOST] [--mem PAGES] [--pids N]
//         [-u HOSTUID] [-d] [CMD ARGS...]
static int
cmd_run(int argc, char *argv[])
{
  const char *name = "ct1", *rootfs = 0, *host = 0, *mem = 0, *pids = 0;
  const char *gwname = "", *ifname = "eth0";
  uint32 ip = 0;
  struct pport ports[NPPORT];
  const char *vols[NCTVOL], *envs[NCTENV];
  int nports = 0, net_host = 0, nvols = 0, nenvs = 0;
  int hostuid = 5000, detach = 0, i = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      name = argv[++i];
    else if (strcmp(argv[i], "--rootfs") == 0 && i + 1 < argc)
      rootfs = argv[++i];
    else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
      host = argv[++i];
    else if (strcmp(argv[i], "--mem") == 0 && i + 1 < argc)
      mem = argv[++i];
    else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc)
      pids = argv[++i];
    else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
      hostuid = atoi(argv[++i]);
    else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      // -i IP[/PREFIX]: an address on a gateway's subnet. Which gateway
      // is decided by the address, or named with --gw.
      char spec[32];
      strncpy(spec, argv[++i], sizeof(spec) - 1);
      spec[sizeof(spec) - 1] = 0;
      char *slash = strchr(spec, '/');
      if (slash)
        *slash = 0;              // the mask is the gateway's
      if (parse_ip(spec, &ip) < 0) {
        fprintf(stderr, "vmm: bad address '%s'\n", spec);
        return 1;
      }
    } else if (strcmp(argv[i], "--gw") == 0 && i + 1 < argc)
      gwname = argv[++i];
    else if (strcmp(argv[i], "--if") == 0 && i + 1 < argc)
      ifname = argv[++i];
    else if (strcmp(argv[i], "--net") == 0 && i + 1 < argc) {
      const char *v = argv[++i];
      if (strcmp(v, "host") == 0)
        net_host = 1;            // the machine's own network, as it was
      else if (strcmp(v, "none") != 0) {
        fprintf(stderr, "vmm: --net host|none\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      if (nports >= NPPORT) {
        fprintf(stderr, "vmm: at most %d published ports\n", NPPORT);
        return 1;
      }
      if (parse_port(argv[++i], &ports[nports]) < 0) {
        fprintf(stderr, "vmm: %s is not a port\n", argv[i]);
        return 1;
      }
      nports++;
    }
    else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
      // -v HOST:PATH[:ro]: a directory of the machine's, inside
      if (nvols >= NCTVOL) {
        fprintf(stderr, "vmm: at most %d volumes\n", NCTVOL);
        return 1;
      }
      vols[nvols++] = argv[++i];
    } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
      if (nenvs >= NCTENV) {
        fprintf(stderr, "vmm: at most %d variables\n", NCTENV);
        return 1;
      }
      envs[nenvs++] = argv[++i];
    } else if (strcmp(argv[i], "-d") == 0)
      detach = 1;
    else
      usage();
  }
  if (rootfs == 0)
    usage();
  if (host == 0)
    host = name;
  char *defcmd[] = {"/bin/sh", 0};
  for (int j = 0; j < nenvs; j++) {
    // the container's environment is this process's, and exec carries it
    char buf[128], *eq;
    strncpy(buf, envs[j], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    if ((eq = strchr(buf, '=')) == 0) {
      fprintf(stderr, "vmm: -e wants VAR=VALUE\n");
      return 1;
    }
    *eq = 0;
    setenv(buf, eq + 1, 1);
  }
  char **cmd = i < argc ? &argv[i] : defcmd;

  int a[2], b[2];
  if (pipe(a) < 0 || pipe(b) < 0) {
    fprintf(stderr, "vmm: pipe: %s\n", strerror(errno));
    return 1;
  }
  int pid = fork();
  if (pid < 0) {
    fprintf(stderr, "vmm: fork: %s\n", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    close(a[0]);
    close(b[1]);
    // into the control group first, while this is still the machine's
    // root: what the container takes is counted from here on
    if (ct_cgroup(name, mem, pids) < 0) {
      fprintf(stderr, "vmm: the group %s: %s\n", name, strerror(errno));
      exit(1);
    }
    // then the network, while this is still the machine's root: a
    // namespace of its own, an address behind a gateway, and the ports
    // it publishes are all the machine's to give, and none of them are
    // the container's to change afterwards.
    if (!net_host && ct_network(ip, gwname, ifname, ports, nports) < 0)
      exit(1);
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWPID) < 0) {
      fprintf(stderr, "vmm: unshare: %s\n", strerror(errno));
      exit(1);
    }
    char c = 'u';
    write(a[1], &c, 1);          // "my namespaces are made"
    if (read(b[0], &c, 1) != 1)  // "your map is written"
      exit(1);
    if (setuid(0) < 0 || setgid(0) < 0) {
      fprintf(stderr, "vmm: setuid: %s\n", strerror(errno));
      exit(1);
    }
    // the next child is the first process of the new pid namespace, and
    // so its init: everything the container starts is under it, and when
    // it goes the container goes.
    int inner = fork();
    if (inner < 0) {
      fprintf(stderr, "vmm: fork: %s\n", strerror(errno));
      exit(1);
    }
    if (inner == 0)
      ct_inside(rootfs, host, cmd, vols, nvols);
    // tell the parent which process is the container: this one unshared
    // the namespaces but is not in the pid namespace it made -- its
    // child is, and is that namespace's init.
    if (write(a[1], &inner, sizeof(inner)) != sizeof(inner))
      ;   // the parent has gone; the wait below ends it either way
    int st = 0;
    while (waitpid(inner, &st, 0) < 0 && errno == EINTR)
      ;
    exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
  }

  close(a[1]);
  close(b[0]);
  char c;
  int inner = 0;
  if (read(a[0], &c, 1) != 1) {
    int st = 0;
    waitpid(pid, &st, 0);
    return 1;                    // it said what went wrong itself
  }
  // its map: inside 0..999 is host `hostuid'.. -- an ordinary user of
  // this machine, which is what root inside a container is.
  char path[64], line[64];
  snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
  snprintf(line, sizeof(line), "0 %d 1000", hostuid);
  if (ct_put(path, line) < 0) {
    fprintf(stderr, "vmm: uid_map: %s\n", strerror(errno));
    kill(pid, SIGKILL);
    return 1;
  }
  snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
  ct_put(path, line);
  c = 'm';
  write(b[1], &c, 1);
  // and now it says which of its processes is the container's init
  if (read(a[0], &inner, sizeof(inner)) != sizeof(inner))
    inner = pid;

  // where it is, for `vmm ps' and `vmm kill'
  mkdir("/run", 0755);
  mkdir("/run/containers", 0755);
  snprintf(path, sizeof(path), "/run/containers/%s", name);
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC, 0644);
  if (fd >= 0) {
    snprintf(line, sizeof(line), "%d\n", inner);
    write(fd, line, strlen(line));
    close(fd);
  }
  if (detach) {
    printf("container %s: pid %d, rootfs %s\n", name, inner, rootfs);
    return 0;
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    ;
  unlink(path);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

// the pid a container's name stands for, or -1
static int
ct_pid(const char *name)
{
  char path[128], buf[32];

  snprintf(path, sizeof(path), "/run/containers/%s", name);
  int fd = open(path, O_RDONLY);
  int n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
  if (fd >= 0)
    close(fd);
  if (n <= 0)
    return -1;
  buf[n] = 0;
  return atoi(buf);
}

// vmm exec NAME CMD ARGS...: a command inside a container that is
// already running.
//
// The order the namespaces are joined in is the order nsenter uses and
// for the same reasons: the user namespace last, because joining it is
// what gives up the machine-wide privilege the others need, and the pid
// namespace just before it, because afterwards a host process number is
// not a number this process can name. Then root inside it, the
// container's own root directory, and the same two one-way promises the
// container's first process made -- a command run in a container is as
// bounded as the container.
static int
cmd_exec(int argc, char *argv[])
{
  if (argc < 2)
    usage();
  const char *name = argv[0];
  int pid = ct_pid(name);
  if (pid < 0) {
    fprintf(stderr, "vmm: no container '%s'\n", name);
    return 1;
  }
  static const struct {
    const char *ns;
    int flag;
  } kinds[] = {
      {"net", CLONE_NEWNET}, {"mnt", CLONE_NEWNS}, {"uts", CLONE_NEWUTS},
      {"pid", CLONE_NEWPID}, {"user", CLONE_NEWUSER},
  };
  int nk = (int)(sizeof(kinds) / sizeof(kinds[0]));
  int fds[8];
  // every one of them is opened before any of them is joined: the path
  // names a process by the number this process can see it by, and after
  // joining its pid namespace that number is not one of those.
  for (int i = 0; i < nk; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/%s", pid, kinds[i].ns);
    if ((fds[i] = open(path, O_RDONLY)) < 0) {
      fprintf(stderr, "vmm: %s: %s\n", path, strerror(errno));
      return 1;
    }
  }
  for (int i = 0; i < nk; i++) {
    if (setns(fds[i], kinds[i].flag) < 0) {
      fprintf(stderr, "vmm: joining its %s: %s\n", kinds[i].ns,
              strerror(errno));
      return 1;
    }
    close(fds[i]);
  }
  // and the command runs in a child, not here: a process that has joined
  // a pid namespace is not *in* it (its own number belongs to whoever is
  // waiting for it), and its children are. A command with no number in
  // the namespace it is running in has no /proc/self and cannot see the
  // processes around it, which is not what `vmm exec' is for.
  int child = fork();
  if (child < 0) {
    fprintf(stderr, "vmm: fork: %s\n", strerror(errno));
    return 1;
  }
  if (child == 0) {
    if (setgid(0) < 0 || setuid(0) < 0) {
      fprintf(stderr, "vmm: root inside: %s\n", strerror(errno));
      _exit(1);
    }
    chdir("/");
    if (sysfilter(SYSFILTER_NO_NEW_PRIVS, 0) < 0 ||
        sysfilter(SYSFILTER_CONTAINER, 0) < 0)
      fprintf(stderr, "vmm: sysfilter: %s\n", strerror(errno));
    execvp(argv[1], argv + 1);
    fprintf(stderr, "vmm: %s: %s\n", argv[1], strerror(errno));
    _exit(1);
  }
  int st = 0;
  while (waitpid(child, &st, 0) < 0 && errno == EINTR)
    ;
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

// vmm ps: the containers that are running
static int
cmd_ctps(void)
{
  DIR *d = opendir("/run/containers");
  struct dirent *e;

  printf("%-16s %8s %8s %8s\n", "NAME", "PID", "PIDS", "MEM");
  if (d == 0)
    return 0;
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] == '.' || e->d_name[0] == 0)
      continue;
    char path[128], buf[32];
    snprintf(path, sizeof(path), "/run/containers/%s", e->d_name);
    int fd = open(path, O_RDONLY);
    int n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0)
      close(fd);
    if (n <= 0)
      continue;
    buf[n] = 0;
    int pid = atoi(buf);
    // gone, if its process is
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
      snprintf(path, sizeof(path), "/run/containers/%s", e->d_name);
      unlink(path);
      continue;
    }
    close(fd);
    char pids[32] = "?", mem[32] = "?";
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/pids.current", e->d_name);
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
      n = read(fd, pids, sizeof(pids) - 1);
      pids[n > 0 ? n : 0] = 0;
      close(fd);
      for (char *q = pids; *q; q++)
        if (*q == '\n')
          *q = 0;
    }
    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/memory.current", e->d_name);
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
      n = read(fd, mem, sizeof(mem) - 1);
      mem[n > 0 ? n : 0] = 0;
      close(fd);
      for (char *q = mem; *q; q++)
        if (*q == '\n')
          *q = 0;
    }
    printf("%-16s %8d %8s %8s\n", e->d_name, pid, pids, mem);
  }
  closedir(d);
  return 0;
}

// vmm stop NAME: the container's init, and everything under it
static int
cmd_stop(const char *name)
{
  char path[128];
  int pid = ct_pid(name);

  snprintf(path, sizeof(path), "/run/containers/%s", name);
  if (pid < 0) {
    fprintf(stderr, "vmm: no container '%s'\n", name);
    return 1;
  }
  if (kill(pid, SIGKILL) < 0) {
    // it has finished by itself, which is not an error: what the name
    // stood for is gone either way, and the file with it
    int gone = (errno == ESRCH);
    unlink(path);
    if (!gone) {
      fprintf(stderr, "vmm: %s: %s\n", name, strerror(errno));
      return 1;
    }
    printf("container %s had already finished\n", name);
    return 0;
  }
  unlink(path);
  printf("container %s stopped\n", name);
  return 0;
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
    fprintf(stderr, "vmm: no vm '%s'\n", s);
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
      fprintf(stderr, "vmm: bad address\n");
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
  if (getinfo(id, &vi) < 0) {
    fprintf(stderr, "vmm: no vm %d\n", id);
    exit(1);
  }
  if (vi.state == VM_FREE) {
    // already gone (a short-lived guest): show what it left in its console.
    char buf[128];
    int n;
    while ((n = vm_cons(id, VMCONS_READ, buf, sizeof(buf))) > 0)
      write(1, buf, n);
    printf("[vm %d has exited]\n", id);
    return;
  }
  printf("[attached to vm %d%s%s, Ctrl-] to detach]\n", id, vi.cfg.name[0] ? " " : "", vi.cfg.name);
  consraw(1);
  int writer = fork();
  if (writer == 0) {
    // keystrokes go to the guest; Ctrl-] asks the reader below to stop.
    char c;
    while (read(0, &c, 1) == 1) {
      if (c == DETACH_CHAR) {
        vm_cons(id, VMCONS_DETACH, 0, 0);
        break;
      }
      if (vm_cons(id, VMCONS_WRITE, &c, 1) < 0)
        break;
    }
    exit(0);
  }
  // guest output until the VM ends or a detach is requested.
  char buf[128];
  int n;
  while ((n = vm_cons(id, VMCONS_READ, buf, sizeof(buf))) > 0)
    write(1, buf, n);
  kill(writer, SIGKILL);
  wait(0);
  consraw(0);
  if (getinfo(id, &vi) == 0 && vi.state == VM_FREE)
    printf("\n[vm %d has exited]\n", id);
  else
    printf("\n[detached from vm %d]\n", id);
}

// the host processes a machine's vcpus run on: one each, and each one
// spends its life inside vm_run. Shared by `start' and `restore'.
static int
spawn_vcpus(int id, int nvcpu)
{
  for (int i = 0; i < nvcpu; i++) {
    int pid = fork();
    if (pid < 0) {
      fprintf(stderr, "vmm: fork failed\n");
      vm_destroy(id);
      return -1;
    }
    if (pid == 0) {
      vm_run(id, i);
      exit(0);
    }
  }
  return 0;
}

// vmm save NAME FILE: the machine, written down (§9).
//
// The file is made here rather than in the kernel: it then belongs to
// whoever ran this, with their umask, and the kernel needs no second
// copy of what open(2) does. A machine that is running is paused for the
// length of the save and left running afterwards.
static int
cmd_save(int argc, char *argv[])
{
  if (argc != 2)
    usage();
  int id = lookup(argv[0]);
  if (id < 0) {
    fprintf(stderr, "vmm: no vm '%s'\n", argv[0]);
    return 1;
  }
  int fd = open(argv[1], O_CREATE | O_WRONLY | O_TRUNC, 0600);
  if (fd < 0) {
    fprintf(stderr, "vmm: %s: %s\n", argv[1], strerror(errno));
    return 1;
  }
  close(fd);
  if (vm_save(id, argv[1]) < 0) {
    fprintf(stderr, "vmm: saving vm %d: %s\n", id, strerror(errno));
    unlink(argv[1]);
    return 1;
  }
  struct stat st;
  if (stat(argv[1], &st) == 0)
    printf("vm %d saved to %s (%ld KB)\n", id, argv[1],
           (long)(st.size / 1024));
  else
    printf("vm %d saved to %s\n", id, argv[1]);
  return 0;
}

// vmm snapshot NAME FILE: the same file as a save, taken while the
// machine keeps running. What it holds is the machine as it was when the
// snapshot began; the guest is stopped only for as long as its registers
// and its devices take to write down.
static int
cmd_snapshot(int argc, char *argv[])
{
  if (argc != 2)
    usage();
  int id = lookup(argv[0]);
  if (id < 0) {
    fprintf(stderr, "vmm: no vm '%s'\n", argv[0]);
    return 1;
  }
  int fd = open(argv[1], O_CREATE | O_WRONLY | O_TRUNC, 0600);
  if (fd < 0) {
    fprintf(stderr, "vmm: %s: %s\n", argv[1], strerror(errno));
    return 1;
  }
  close(fd);
  if (vm_snapshot(id, argv[1]) < 0) {
    fprintf(stderr, "vmm: snapshotting vm %d: %s\n", id, strerror(errno));
    unlink(argv[1]);
    return 1;
  }
  struct stat st;
  if (stat(argv[1], &st) == 0)
    printf("vm %d snapshotted to %s (%ld KB), still running\n", id, argv[1],
           (long)(st.size / 1024));
  else
    printf("vm %d snapshotted to %s, still running\n", id, argv[1]);
  return 0;
}

// vmm clone BASE NEW [-f DISK] [-d]: the same machine again, sharing the
// original's pages until one of them writes.
//
// The disk is the caller's to say: two machines with one image file open
// are two kernels writing one file system, so a clone gets a disk of its
// own -- a copy, or an overlay over the original's image, which is what
// `losetup -b BASE DEV DELTA' makes.
static int
cmd_clone(int argc, char *argv[])
{
  const char *disk = "";
  int detach = 0, n = 0;
  const char *names[2] = {0, 0};

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
      disk = argv[++i];
    else if (strcmp(argv[i], "-d") == 0)
      detach = 1;
    else if (argv[i][0] != '-' && n < 2)
      names[n++] = argv[i];
    else
      usage();
  }
  if (n != 2)
    usage();
  int id = lookup((char *)names[0]);
  if (id < 0) {
    fprintf(stderr, "vmm: no vm '%s'\n", names[0]);
    return 1;
  }
  int nid = vm_clone(id, names[1], disk);
  if (nid < 0) {
    fprintf(stderr, "vmm: cloning vm %d: %s\n", id, strerror(errno));
    return 1;
  }
  struct vminfo vi;
  if (getinfo(nid, &vi) < 0) {
    fprintf(stderr, "vmm: vm %d?\n", nid);
    return 1;
  }
  printf("vm %d %s cloned from %s: %d vcpu(s), %d MB shared\n", nid,
         names[1], names[0], vi.nvcpu, vi.mem_mb);
  if (spawn_vcpus(nid, vi.nvcpu) < 0)
    return 1;
  if (!detach)
    attach(nid);
  return 0;
}

// vmm migrate VM HOST:PORT: the machine, over a connection, to another
// xv6H host (§9).
//
// The connection is made here, in a program, with the ordinary socket
// calls; the kernel writes the machine into it. What crosses is the same
// sections a save writes to a file, so a machine that can be saved can be
// migrated and the other end needs no special reader.
//
// It is a stop-and-copy: the guest is paused for as long as its memory
// takes to cross. Iterative pre-copy -- sending the memory while the guest
// runs and then only what it dirtied -- is what §9 asks for and is not
// here; the snapshot machinery from E9 is what it would be built on.
static int
cmd_migrate(int argc, char *argv[])
{
  if (argc != 2)
    usage();
  int id = lookup(argv[0]);
  if (id < 0) {
    fprintf(stderr, "vmm: no vm '%s'\n", argv[0]);
    return 1;
  }
  char spec[64], *colon;
  strncpy(spec, argv[1], sizeof(spec) - 1);
  spec[sizeof(spec) - 1] = 0;
  if ((colon = strchr(spec, ':')) == 0) {
    fprintf(stderr, "vmm: migrate VM HOST:PORT\n");
    return 1;
  }
  *colon = 0;
  uint32 ip = 0;
  if (parse_ip(spec, &ip) < 0) {
    fprintf(stderr, "vmm: %s is not an address\n", spec);
    return 1;
  }
  int port = atoi(colon + 1);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fprintf(stderr, "vmm: socket: %s\n", strerror(errno));
    return 1;
  }
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16)port);
  a.sin_addr.s_addr = htonl(ip);
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    fprintf(stderr, "vmm: connect to %s: %s\n", argv[1], strerror(errno));
    close(fd);
    return 1;
  }
  if (vm_migrate(id, fd) < 0) {
    fprintf(stderr, "vmm: migrating vm %d: %s\n", id, strerror(errno));
    close(fd);
    return 1;
  }
  close(fd);
  printf("vm %d migrated to %s\n", id, argv[1]);
  return 0;
}

// vmm accept PORT [-n NAME] [-d]: wait for a machine to arrive.
static int
cmd_accept(int argc, char *argv[])
{
  const char *name = "";
  int port = 0, detach = 0;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      name = argv[++i];
    else if (strcmp(argv[i], "-d") == 0)
      detach = 1;
    else if (argv[i][0] != '-' && port == 0)
      port = atoi(argv[i]);
    else
      usage();
  }
  if (port <= 0)
    usage();
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) {
    fprintf(stderr, "vmm: socket: %s\n", strerror(errno));
    return 1;
  }
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16)port);
  a.sin_addr.s_addr = htonl(0);
  if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 1) < 0) {
    fprintf(stderr, "vmm: listening on %d: %s\n", port, strerror(errno));
    close(ls);
    return 1;
  }
  printf("waiting for a machine on port %d\n", port);
  int fd = accept(ls, 0, 0);
  if (fd < 0) {
    fprintf(stderr, "vmm: accept: %s\n", strerror(errno));
    close(ls);
    return 1;
  }
  int id = vm_accept(fd, name);
  close(fd);
  close(ls);
  if (id < 0) {
    fprintf(stderr, "vmm: taking the machine: %s\n", strerror(errno));
    return 1;
  }
  struct vminfo vi;
  if (getinfo(id, &vi) < 0) {
    fprintf(stderr, "vmm: vm %d?\n", id);
    return 1;
  }
  printf("vm %d%s%s arrived: %d vcpu(s), %d MB\n", id,
         vi.cfg.name[0] ? " " : "", vi.cfg.name, vi.nvcpu, vi.mem_mb);
  if (spawn_vcpus(id, vi.nvcpu) < 0)
    return 1;
  if (!detach)
    attach(id);
  return 0;
}

// vmm restore FILE [-n NAME] [-d]: the machine again, going on from
// where it stopped.
static int
cmd_restore(int argc, char *argv[])
{
  const char *name = "";
  const char *path = 0;
  int detach = 0;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      name = argv[++i];
    else if (strcmp(argv[i], "-d") == 0)
      detach = 1;
    else if (argv[i][0] != '-' && path == 0)
      path = argv[i];
    else
      usage();
  }
  if (path == 0)
    usage();
  int id = vm_restore(path, name);
  if (id < 0) {
    fprintf(stderr, "vmm: restoring %s: %s\n", path, strerror(errno));
    return 1;
  }
  struct vminfo vi;
  if (getinfo(id, &vi) < 0) {
    fprintf(stderr, "vmm: vm %d?\n", id);
    return 1;
  }
  printf("vm %d%s%s restored from %s: %d vcpu(s), %d MB\n", id,
         vi.cfg.name[0] ? " " : "", vi.cfg.name, path, vi.nvcpu, vi.mem_mb);
  if (spawn_vcpus(id, vi.nvcpu) < 0)
    return 1;
  if (!detach)
    attach(id);
  return 0;
}

// create and start a VM from cfg; returns its id.
static int
launch(struct vmconfig *cfg)
{
  int id = vm_create(cfg);
  if (id < 0) {
    fprintf(stderr, "vmm: cannot create vm (kernel/disk missing, disk in use, no memory, or no hypervisor?)\n");
    return -1;
  }
  if (spawn_vcpus(id, cfg->nvcpu) < 0)
    return -1;
  printf("vm %d%s%s: %d vcpu(s), %d MB, kernel %s, disk %s\n", id, cfg->name[0] ? " " : "",
         cfg->name, cfg->nvcpu, cfg->mem_mb, cfg->kernel, cfg->disk[0] ? cfg->disk : "none");
  return id;
}

static void
setstr(char *dst, int max, char *src)
{
  int n = strlen(src);
  if (n >= max) {
    fprintf(stderr, "vmm: '%s' is too long\n", src);
    exit(1);
  }
  memmove(dst, src, n + 1);
}

static void
append_bootarg(struct vmconfig *cfg, char *w)
{
  int len = strlen(cfg->bootargs), n = strlen(w);
  if (len + n + 2 > sizeof(cfg->bootargs)) {
    fprintf(stderr, "vmm: bootargs too long\n");
    exit(1);
  }
  if (len)
    cfg->bootargs[len++] = ' ';
  memmove(cfg->bootargs + len, w, n + 1);
}

// -i dhcp | IP/PREFIX[,GW[,DNS]]: the guest's address configuration.
// an xv6 guest reads it from its device tree; a Linux guest gets the
// equivalent ip= kernel parameter.
static void
parse_net(struct vmconfig *cfg, char *spec)
{
  if (strcmp(spec, "dhcp") == 0) {
    cfg->net = 1;
    return;
  }
  char buf[64], *p = buf, *q;
  setstr(buf, sizeof(buf), spec);
  q = strchr(p, ',');
  if (q)
    *q++ = 0;
  char *slash = strchr(p, '/');
  int prefix = 24;
  if (slash) {
    *slash = 0;
    prefix = atoi(slash + 1);
  }
  if (parse_ip(p, &cfg->ip) < 0 || prefix < 1 || prefix > 30) {
    fprintf(stderr, "vmm: bad address '%s'\n", spec);
    exit(1);
  }
  cfg->mask = ~0U << (32 - prefix);
  cfg->gw = (cfg->ip & cfg->mask) | 2; // qemu's user-mode gateway is .2
  cfg->dns = (cfg->ip & cfg->mask) | 3; // and its dns forwarder .3
  if (q) {
    p = q;
    q = strchr(p, ',');
    if (q)
      *q++ = 0;
    if (parse_ip(p, &cfg->gw) < 0) {
      fprintf(stderr, "vmm: bad gateway '%s'\n", p);
      exit(1);
    }
    if (q && parse_ip(q, &cfg->dns) < 0) {
      fprintf(stderr, "vmm: bad dns '%s'\n", q);
      exit(1);
    }
  }
  cfg->net = 2;
}

static void
net_bootargs(struct vmconfig *cfg)
{
  if (cfg->net == 1) {
    append_bootarg(cfg, "ip=dhcp");
  } else if (cfg->net == 2) {
    char a[16], g[16], m[16], d[16], arg[96];
    ip2str(cfg->ip, a);
    ip2str(cfg->gw, g);
    ip2str(cfg->mask, m);
    ip2str(cfg->dns, d);
    // ip=<client>:<server>:<gw>:<netmask>:<hostname>:<device>:<autoconf>:<dns0>
    int n = 0;
    char *parts[] = {"ip=", a, "::", g, ":", m, "::eth0:none:", d};
    for (int i = 0; i < 8; i++) {
      int k = strlen(parts[i]);
      memmove(arg + n, parts[i], k);
      n += k;
    }
    arg[n] = 0;
    append_bootarg(cfg, arg);
  }
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
    } else if (strcmp(a, "-M") == 0 && v) {
      cfg.mem_limit_mb = atoi(v);
      i++;
    } else if (strcmp(a, "-w") == 0 && v) {
      // the machine's share of the host's processors, whatever its
      // vcpu count: 1024 is one nice-0 task's worth
      cfg.weight = atoi(v);
      i++;
    } else if (strcmp(a, "--cpu-max") == 0 && v) {
      // and a ceiling, in hundredths of one processor
      cfg.cpu_max_us = atoi(v) * 1000;   // per 100 ms
      cfg.cpu_period_us = 100000;
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
          fprintf(stderr, "vmm: bootargs too long\n");
          exit(1);
        }
        if (len)
          cfg.bootargs[len++] = ' ';
        memmove(cfg.bootargs + len, w, n);
        len += n;
        cfg.bootargs[len] = 0;
      }
    } else if (strcmp(a, "-i") == 0 && v) {
      parse_net(&cfg, v);
      i++;
    } else if (strcmp(a, "--gw") == 0 && v) {
      setstr(cfg.gwname, sizeof(cfg.gwname), v);
      i++;
    } else if (strcmp(a, "--nic") == 0 && v) {
      // --nic [NAME:]IP/PREFIX[,gw=GW][,dns=DNS] -- the same as -i,
      // spelled the way a machine behind a gateway is described. The
      // gateway address decides which gateway it is behind.
      char nb[64];
      strncpy(nb, v, sizeof(nb) - 1);
      nb[sizeof(nb) - 1] = 0;
      char *spec = nb;
      char *colon = strchr(nb, ':');
      if (colon)
        spec = colon + 1;          // the interface name is the guest's own
      char *comma;
      char rest[64];
      rest[0] = 0;
      if ((comma = strchr(spec, ',')) != 0) {
        *comma = 0;
        strncpy(rest, comma + 1, sizeof(rest) - 1);
        rest[sizeof(rest) - 1] = 0;
      }
      char netspec[96];
      strcpy(netspec, spec);
      // gw=A,dns=B in either order, which parse_net wants as ,A,B
      char gwbuf[24] = "", dnsbuf[24] = "";
      for (char *tok = rest; tok && *tok;) {
        char *next = strchr(tok, ',');
        if (next)
          *next++ = 0;
        if (strncmp(tok, "gw=", 3) == 0)
          strncpy(gwbuf, tok + 3, sizeof(gwbuf) - 1);
        else if (strncmp(tok, "dns=", 4) == 0)
          strncpy(dnsbuf, tok + 4, sizeof(dnsbuf) - 1);
        else {
          fprintf(stderr, "vmm: --nic: %s?\n", tok);
          exit(1);
        }
        tok = next;
      }
      if (gwbuf[0]) {
        strcat(netspec, ",");
        strcat(netspec, gwbuf);
        if (dnsbuf[0]) {
          strcat(netspec, ",");
          strcat(netspec, dnsbuf);
        }
      }
      parse_net(&cfg, netspec);
      i++;
    } else if (strcmp(a, "-p") == 0 && v) {
      if (cfg.nports >= NPPORT) {
        fprintf(stderr, "vmm: at most %d published ports\n", NPPORT);
        exit(1);
      }
      if (parse_port(v, &cfg.ports[cfg.nports]) < 0) {
        fprintf(stderr, "vmm: %s is not a port\n", v);
        exit(1);
      }
      cfg.nports++;
      i++;
    } else if (strcmp(a, "-H") == 0) {
      cfg.flags |= VMF_HSM;
    } else if (strcmp(a, "-s") == 0) {
      cfg.flags |= VMF_SOFTIMSIC;
    } else if (strcmp(a, "-4") == 0) {
      cfg.flags |= VMF_NOHUGE;
    } else if (strcmp(a, "-Q") == 0) {
      cfg.flags |= VMF_CONQUEUE;
    } else if (strcmp(a, "-d") == 0) {
      detach = 1;
    } else {
      usage();
    }
  }
  if (cfg.nvcpu <= 0 || cfg.mem_mb <= 0)
    usage();
  if (cfg.name[0] && lookup(cfg.name) >= 0) {
    fprintf(stderr, "vmm: a vm named '%s' already exists\n", cfg.name);
    exit(1);
  }
  net_bootargs(&cfg);

  int id = launch(&cfg);
  if (id < 0)
    exit(1);
  if (!detach)
    attach(id);
  exit(0);
}

static void
pad(const char *s, int width)
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
  printf("vm %d: name '%s' state %s, %d vcpu(s), %d MB (%d MB resident), intc %s%s\n", id, vi.cfg.name,
         states[vi.state % 5], vi.nvcpu, vi.mem_mb, vi.resident_mb, intcs[vi.intc % 3],
         vi.imsic_hw ? " (hardware guest files)" : "");
  printf("  memory: %d chunks split to pages, %d table pages%s%s",
         vi.split_chunks, vi.table_pages,
         (vi.cfg.flags & VMF_NOHUGE) ? ", 4 KB pages only" : "",
         (vi.cfg.flags & VMF_CONQUEUE) ? ", console queue" : "");
  if (vi.mem_limit_mb)
    printf(", limit %d MB", vi.mem_limit_mb);
  printf("\n");
  printf("  cpu: weight %d", vi.weight);
  if (vi.cpu_max_us > 0 && vi.cpu_period_us > 0)
    printf(", at most %d%% of one processor (%ld throttle%s)",
           vi.cpu_max_us * 100 / vi.cpu_period_us, vi.throttles,
           vi.throttles == 1 ? "" : "s");
  if (vi.frozen)
    printf(", frozen");
  printf("\n");
  if (vi.balloon_target_mb || vi.balloon_actual_mb)
    printf("  balloon: %d MB asked for, %d MB given back\n",
           vi.balloon_target_mb, vi.balloon_actual_mb);
  printf("  kernel %s%s entry 0x%lx, disk %s (%ld sectors)%s\n", vi.cfg.kernel,
         (vi.cfg.flags & VMF_RAW) ? " (raw)" : "", vi.entry,
         vi.cfg.disk[0] ? vi.cfg.disk : "none", vi.disk_sectors,
         (vi.cfg.flags & VMF_HSM) ? ", hsm boot" : "");
  if (vi.cfg.bootargs[0])
    printf("  bootargs: %s\n", vi.cfg.bootargs);
  if (vi.cfg.net == 1) {
    printf("  net: dhcp\n");
  } else if (vi.cfg.net == 2) {
    char a[16], m[16], g[16], d[16];
    printf("  net: static %s mask %s gw %s dns %s\n", ip2str(vi.cfg.ip, a), ip2str(vi.cfg.mask, m),
           ip2str(vi.cfg.gw, g), ip2str(vi.cfg.dns, d));
  } else {
    printf("  net: guest default (xv6: dhcp)\n");
  }
  printf("  exits %ld: mmio %ld, csr %ld, wfi sleeps %ld, injected %ld", vi.exits, vi.mmio, vi.csr,
         vi.wfi, vi.injected);
  // and which device they went to. Eighty-two per cent of a guest's exits
  // being emulated accesses only names the work if it says *whose*.
  {
    int shown = 0;
    for (int i = 0; i < vi.ndev; i++) {
      if (vi.dev[i].loads + vi.dev[i].stores == 0)
        continue;
      printf("%s%s %ldr/%ldw", shown++ ? ", " : "\n  by device: ",
             vi.dev[i].name, vi.dev[i].loads, vi.dev[i].stores);
      if (vi.dev[i].irqs)
        printf("/%ldi", vi.dev[i].irqs);
    }
  }
  // and what vcpu 0's exits were, which is what says where its time goes
  {
    static const char *const kinds[] = {"mmio-load", "mmio-store",
                                        "inst-fault", "csr",  "ecall",
                                        "illegal",    "timer", "external",
                                        "other"};
    int any = 0;
    for (int k = 0; k < 9; k++)
      if (vi.exit_hist[k])
        any = 1;
    if (any) {
      printf("\n  vcpu0 exits by cause:");
      for (int k = 0; k < 9; k++)
        if (vi.exit_hist[k])
          printf(" %s=%ld", kinds[k], vi.exit_hist[k]);
    }
  }
  if (vi.exit_throttles)
    printf(", %ld exit-rate throttle%s", vi.exit_throttles,
           vi.exit_throttles == 1 ? "" : "s");
  if (vi.nudges)
    printf(", %ld sibling nudge%s", vi.nudges, vi.nudges == 1 ? "" : "s");
  printf("\n");
  printf("  vcpu0: pc 0x%lx last cause 0x%lx gpa 0x%lx hart %d idle %d mmode %d\n", vi.pc,
         vi.last_cause, vi.last_gpa, vi.running_hart, vi.idle, vi.mmode);
  printf("         vsatp 0x%lx vstvec 0x%lx vsepc 0x%lx vscause 0x%lx vsstatus 0x%lx vstval 0x%lx\n",
         vi.vsatp, vi.vstvec, vi.vsepc, vi.vscause, vi.vsstatus, vi.vstval);
  printf("         sp 0x%lx ra 0x%lx vsie 0x%lx vstimecmp 0x%lx\n", vi.sp, vi.ra, vi.vsie,
         vi.vstimecmp);
  printf("         scounteren 0x%lx senvcfg 0x%lx hcounteren 0x%lx\n", vi.scounteren, vi.senvcfg,
         vi.hcounteren);
  printf("         imsic0: deliv %d thr %d eie0 0x%lx eip0 0x%lx\n", vi.deliv0, vi.thr0, vi.eie0_0, vi.eip0_0);
  if (vi.nvcpu > 1)
    printf("  vcpu1: pc 0x%lx last cause 0x%lx exits %ld idle %d hsm %d hart %d\n"
           "         imsic1: deliv %d thr %d eie0 0x%lx eip0 0x%lx\n",
           vi.pc1, vi.cause1, vi.exits1, vi.idle1, vi.hsm1, vi.hart1, vi.deliv1, vi.thr1, vi.eie0_1,
           vi.eip0_1);
}

static void
stats(void)
{
  uint64 s[7 + MAXVM];
  memset(s, 0, sizeof(s));
  if (vm_stats(s, 7 + MAXVM) < 0) {
    fprintf(stderr, "vmm: vm_stats failed\n");
    exit(1);
  }
  // in pages as well as megabytes: a megabyte is 256 pages, and
  // anything looking for a leak needs the finer number
  printf("free guest memory: %ld MB (%ld pages), ipis: %ld, exits:", s[0],
         s[2 + MAXVM], s[1]);
  for (int i = 0; i < MAXVM; i++)
    if (s[2 + i])
      printf(" vm%d=%ld", i, s[2 + i]);
  printf("\n");
  printf("same-page merging: %s, %ld pages scanned, %ld merged, %ld shared\n",
         s[3 + MAXVM] ? "on" : "off", s[4 + MAXVM], s[5 + MAXVM],
         s[6 + MAXVM]);
}

static void
wait_freed(int id)
{
  struct vminfo vi;
  for (int i = 0; i < 100; i++) {
    if (getinfo(id, &vi) < 0 || vi.state == VM_FREE)
      return;
    ticksleep(1);
  }
  fprintf(stderr, "vmm: vm %d did not stop\n", id);
  exit(1);
}

static void
killvm(int id)
{
  if (vm_destroy(id) < 0) {
    fprintf(stderr, "vmm: no vm %d\n", id);
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
  // half a second, not fifty milliseconds: the guest prints a line per
  // process through an emulated 8250, and on a busy machine a list of a
  // dozen of them arrives in pieces. Cutting the reader off early showed
  // the first few and left the rest, which is how `vmm ps' came to print
  // a list with its own init missing.
  ticksleep(50);
  kill(reader, SIGKILL);
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
  } else if (strcmp(cmd, "port") == 0) {
    exit(cmd_port(argc - 2, argv + 2));
  } else if (strcmp(cmd, "gw") == 0) {
    exit(cmd_gw(argc - 2, argv + 2));
  } else if (strcmp(cmd, "run") == 0) {
    exit(cmd_run(argc - 2, argv + 2));
  } else if (strcmp(cmd, "save") == 0) {
    exit(cmd_save(argc - 2, argv + 2));
  } else if (strcmp(cmd, "snapshot") == 0) {
    exit(cmd_snapshot(argc - 2, argv + 2));
  } else if (strcmp(cmd, "migrate") == 0) {
    exit(cmd_migrate(argc - 2, argv + 2));
  } else if (strcmp(cmd, "accept") == 0) {
    exit(cmd_accept(argc - 2, argv + 2));
  } else if (strcmp(cmd, "clone") == 0) {
    exit(cmd_clone(argc - 2, argv + 2));
  } else if (strcmp(cmd, "restore") == 0) {
    exit(cmd_restore(argc - 2, argv + 2));
  } else if (strcmp(cmd, "exec") == 0) {
    exit(cmd_exec(argc - 2, argv + 2));
  } else if (strcmp(cmd, "ps") == 0 && argc == 2) {
    exit(cmd_ctps());
  } else if (strcmp(cmd, "stop") == 0 && argc == 3) {
    exit(cmd_stop(argv[2]));
  } else if (argc == 3 && strcmp(cmd, "kill") == 0 &&
             (strcmp(argv[2], "--all") == 0 || strcmp(argv[2], "-a") == 0)) {
    killall();
  } else if (argc == 3 && strcmp(cmd, "ksm") == 0) {
    int on = strcmp(argv[2], "on") == 0;
    if (!on && strcmp(argv[2], "off") != 0)
      usage();
    if (vm_ctl(0, on ? VMCTL_KSM_ON : VMCTL_KSM_OFF, 0) < 0) {
      fprintf(stderr, "vmm: cannot turn same-page merging %s\n", argv[2]);
      exit(1);
    }
    printf("same-page merging %s\n", on ? "on" : "off");
  } else if (argc == 4 && strcmp(cmd, "weight") == 0) {
    int id = lookup(argv[2]);
    int w = atoi(argv[3]);
    if (id < 0) {
      fprintf(stderr, "vmm: no vm '%s'\n", argv[2]);
      exit(1);
    }
    if (vm_ctl(id, VMCTL_WEIGHT, w) < 0) {
      fprintf(stderr, "vmm: weight %d refused\n", w);
      exit(1);
    }
    printf("vm %d weight %d\n", id, w);
  } else if (argc == 4 && strcmp(cmd, "cpumax") == 0) {
    int id = lookup(argv[2]);
    int pct = atoi(argv[3]);
    if (id < 0) {
      fprintf(stderr, "vmm: no vm '%s'\n", argv[2]);
      exit(1);
    }
    if (vm_ctl(id, VMCTL_CPUMAX, pct) < 0) {
      fprintf(stderr, "vmm: cpu ceiling %d%% refused\n", pct);
      exit(1);
    }
    if (pct)
      printf("vm %d at most %d%% of one processor\n", id, pct);
    else
      printf("vm %d no processor ceiling\n", id);
  } else if (argc == 4 && strcmp(cmd, "balloon") == 0) {
    int id = must_lookup(argv[2]);
    int mb = atoi(argv[3]);
    if (mb < 0)
      usage();
    if (vm_ctl(id, VMCTL_BALLOON, mb) < 0) {
      fprintf(stderr, "vmm: vm %d has no balloon\n", id);
      exit(1);
    }
    printf("vm %d: asked for %d MB back\n", id, mb);
  } else if (argc == 3) {
    int id = must_lookup(argv[2]);
    if (strcmp(cmd, "attach") == 0)
      attach(id);
    else if (strcmp(cmd, "kill") == 0)
      killvm(id);
    else if (strcmp(cmd, "restart") == 0)
      restart(id);
    else if (strcmp(cmd, "pause") == 0)
      printf("vm %d %s\n", id, vm_ctl(id, VMCTL_PAUSE, 0) == 0 ? "paused" : "not paused");
    else if (strcmp(cmd, "resume") == 0)
      printf("vm %d %s\n", id, vm_ctl(id, VMCTL_RESUME, 0) == 0 ? "resumed" : "not resumed");
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
