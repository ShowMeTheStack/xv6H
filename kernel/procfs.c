//
// /proc: the kernel's state as files.
//
// nothing here is stored. a file's contents are generated when it is
// read, into a buffer the size of one page, and the read then takes
// its slice of that. this is how Linux's procfs works for its small
// files, and it keeps the file system itself to a table of names and
// the functions behind them.
//
// the inode numbers are made up: a fixed number for each of the files
// at the top level, and for a process's own directory the pid shifted
// up with the file's index in the low bits, so an inode number says
// what it refers to without a table.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "slab.h"
#include "defs.h"
#include "blk.h"
#include "vfs.h"
#include "page.h"
#include "mm.h"
#include "platform.h"
#include "errno.h"
#include "nsproxy.h"
#include "net.h"
#include "cgw.h"
#include "sock.h"
#include "vmm.h"
#include "tcp.h"
#include "unixsock.h"

#define PROCFS_DEV 32

// the inode numbers of what lives at the top level
#define PINO_ROOT      1
#define PINO_MOUNTS    2
#define PINO_MEMINFO   3
#define PINO_CPUINFO   4
#define PINO_UPTIME    5
#define PINO_VERSION   6
#define PINO_FILESYSTEMS 7
#define PINO_PARTITIONS  8
#define PINO_DISKSTATS   9
#define PINO_SELF       10
#define PINO_STAT       11
// /proc/net, and what is in it. these are the files netstat, ss, arp
// and route read, in the shapes those programs expect.
#define PINO_NET        12
#define PINO_NET_DEV    13
#define PINO_NET_ARP    14
#define PINO_NET_ROUTE  15
#define PINO_NET_TCP    16
#define PINO_NET_UDP    17
#define PINO_NET_SOCKSTAT 18
#define PINO_NET_UNIX   19
#define PINO_SCHEDSTAT  20
#define PINO_SLABINFO   21
#define PINO_VMM        22
#define PINO_AUDIT      23
#define PINO_NET_CONNTRACK 24
#define PINO_NET_GATEWAYS  25
#define PINO_FIRST_PID 1024

// a process's files: the inode number is (pid << 4) + index + base
#define PPID_DIR     0
#define PPID_STATUS  1
#define PPID_CMDLINE 2
#define PPID_STAT    3
#define PPID_MAPS    4
#define PPID_FD      5
#define PPID_CWD     6
#define PPID_ROOT    7
#define PPID_NS      8
#define PPID_UIDMAP  9
#define PPID_GIDMAP 10
#define PPID_NFILES 11

static const char *const pid_files[PPID_NFILES] = {
    0, "status", "cmdline", "stat", "maps", "fd", "cwd", "root", "ns",
    "uid_map", "gid_map"};

// one inode per open descriptor, in a space of its own: the top bit
// marks it, then the process and the descriptor number. a process may
// hold up to 1024 files, which is what the descriptor table grows to.
#define PINO_FD_FLAG 0x80000000u
#define PINO_FD_SHIFT 10

static uint
fd_inum(int pid, int fd)
{
  return PINO_FD_FLAG | ((uint)pid << PINO_FD_SHIFT) | ((uint)fd & 0x3ff);
}

static int
is_fd_inum(uint inum, int *pid, int *fd)
{
  if ((inum & PINO_FD_FLAG) == 0)
    return 0;
  *pid = (int)((inum & ~PINO_FD_FLAG) >> PINO_FD_SHIFT);
  *fd = (int)(inum & 0x3ff);
  return 1;
}

// /proc/<pid>/ns/<kind>: one file per namespace the process is in, whose
// contents name the namespace ("uts:[3]"), as Linux's do. Linux makes
// them symbolic links into a file system of its own; here they are
// ordinary files, because what matters is that a descriptor for one
// names a namespace, and setns(2) takes such a descriptor.
#define PINO_NS_FLAG 0x40000000u
#define PINO_NS_SHIFT 4

static uint
ns_inum(int pid, int kind)
{
  return PINO_NS_FLAG | ((uint)pid << PINO_NS_SHIFT) | ((uint)kind & 0xf);
}

static int
is_ns_inum(uint inum, int *pid, int *kind)
{
  if ((inum & PINO_NS_FLAG) == 0 || (inum & PINO_FD_FLAG) != 0)
    return 0;
  *pid = (int)((inum & ~PINO_NS_FLAG) >> PINO_NS_SHIFT);
  *kind = (int)(inum & 0xf);
  return 1;
}


struct topfile {
  const char *name;
  uint inum;
};

static const struct topfile topfiles[] = {
    {"mounts", PINO_MOUNTS},         {"meminfo", PINO_MEMINFO},
    {"cpuinfo", PINO_CPUINFO},       {"uptime", PINO_UPTIME},
    {"version", PINO_VERSION},       {"filesystems", PINO_FILESYSTEMS},
    {"partitions", PINO_PARTITIONS}, {"diskstats", PINO_DISKSTATS},
    {"self", PINO_SELF},             {"stat", PINO_STAT},
    {"schedstat", PINO_SCHEDSTAT}, {"slabinfo", PINO_SLABINFO},
    {"vmm", PINO_VMM},           {"audit", PINO_AUDIT},
    {"net", PINO_NET},
    {0, 0},
};

// what is in /proc/net
static const struct topfile netfiles[] = {
    {"dev", PINO_NET_DEV},     {"arp", PINO_NET_ARP},
    {"route", PINO_NET_ROUTE}, {"tcp", PINO_NET_TCP},
    {"udp", PINO_NET_UDP},     {"sockstat", PINO_NET_SOCKSTAT},
    {"unix", PINO_NET_UNIX}, {"conntrack", PINO_NET_CONNTRACK},
    {"gateways", PINO_NET_GATEWAYS},
    {0, 0},
};

static const struct inode_ops procfs_iops;
static struct super *proc_sb;

// ---- generating a file ----

// a small printf into a fixed buffer, which is all these files need
struct gen {
  char *buf;
  int len, cap;
};

static void
gen_str(struct gen *g, const char *s)
{
  while (*s && g->len < g->cap - 1)
    g->buf[g->len++] = *s++;
}

static void
gen_num(struct gen *g, uint64 v)
{
  char tmp[24];
  int n = 0;

  if (v == 0)
    tmp[n++] = '0';
  while (v) {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  }
  while (n > 0 && g->len < g->cap - 1)
    g->buf[g->len++] = tmp[--n];
}

static void
gen_kv(struct gen *g, const char *key, uint64 value, const char *unit)
{
  gen_str(g, key);
  gen_str(g, ":\t");
  gen_num(g, value);
  if (unit) {
    gen_str(g, " ");
    gen_str(g, unit);
  }
  gen_str(g, "\n");
}

static void
mounts_emit(const char *source, const char *path, const char *type, int readonly, void *arg)
{
  struct gen *g = arg;

  gen_str(g, source);
  gen_str(g, " ");
  gen_str(g, path);
  gen_str(g, " ");
  gen_str(g, type);
  gen_str(g, readonly ? " ro 0 0\n" : " rw 0 0\n");
}

extern uint ticks;

// ---- /proc/net ----

// a number in hex, zero-padded, which is how Linux writes the
// addresses and ports in these files
static void
gen_hex(struct gen *g, uint64 v, int digits)
{
  static const char *hex = "0123456789ABCDEF";

  for (int i = digits - 1; i >= 0; i--)
    if (g->len < g->cap - 1)
      g->buf[g->len++] = hex[(v >> (i * 4)) & 15];
}

// an address as /proc/net writes it: four bytes, least significant
// first, in hex -- which is the host's word order on a little-endian
// machine, and is what every tool that reads these files expects
static void
gen_addr_le(struct gen *g, uint32 ip)
{
  gen_hex(g, ((ip & 0xff) << 24) | ((ip & 0xff00) << 8) | ((ip >> 8) & 0xff00) |
                 (ip >> 24),
          8);
}

static void
gen_dotted(struct gen *g, uint32 ip)
{
  for (int i = 3; i >= 0; i--) {
    gen_num(g, (ip >> (i * 8)) & 0xff);
    if (i)
      gen_str(g, ".");
  }
}

struct arpctx {
  struct gen *g;
};

static void
arp_emit(uint32 ip, const uint8 *mac, int valid, void *arg)
{
  struct arpctx *c = arg;
  char macs[18];

  gen_dotted(c->g, ip);
  // the columns are Linux's: address, type, flags, hardware address,
  // mask, device
  gen_str(c->g, "\t0x1\t");
  gen_str(c->g, valid ? "0x2" : "0x0");
  gen_str(c->g, "\t");
  gen_str(c->g, mac2str(mac, macs));
  gen_str(c->g, "\t*\teth0\n");
}

// the TCP state numbers Linux uses in /proc/net/tcp, which are not the
// ones this stack numbers its own states with
static int
linux_tcp_state(int st)
{
  switch (st) {
  case TCPS_ESTABLISHED: return 1;
  case TCPS_SYN_SENT: return 2;
  case TCPS_SYN_RCVD: return 3;
  case TCPS_FIN_WAIT_1: return 4;
  case TCPS_FIN_WAIT_2: return 5;
  case TCPS_TIME_WAIT: return 6;
  case TCPS_CLOSED: return 7;
  case TCPS_CLOSE_WAIT: return 8;
  case TCPS_LAST_ACK: return 9;
  case TCPS_LISTEN: return 10;
  case TCPS_CLOSING: return 11;
  }
  return 7;
}

struct sockctx {
  struct gen *g;
  int want;      // SOCK_STREAM or SOCK_DGRAM
  int line;
  int counts[SOCK_NTYPES];
};

// one AF_UNIX socket, in the shape Linux's /proc/net/unix has: an
// identifier, the reference count, the protocol (always zero), the
// flags (0x10000 for one that accepts connections), the type, the
// state, an inode number and the name, if it has one. an abstract
// name is written with an @ where its leading zero byte is, which is
// what Linux does and what makes it readable at all.
static void
unix_emit(const struct unsockinfo *si, void *arg)
{
  struct gen *g = arg;

  gen_hex(g, 0xffff0000u | (uint)si->index, 8);
  gen_str(g, ": ");
  gen_hex(g, si->refs, 8);
  gen_str(g, " 00000000 ");
  gen_hex(g, si->listening ? 0x10000 : 0, 8);
  gen_str(g, " ");
  gen_hex(g, si->type, 4);
  gen_str(g, " ");
  // 01 is unconnected (which is what a listening socket shows as),
  // 03 is connected; there is nothing in between here
  gen_hex(g, si->connected ? 3 : 1, 2);
  gen_str(g, " ");
  gen_num(g, (uint64)si->index + 1);
  if (si->pathlen > 0) {
    gen_str(g, " ");
    for (int i = 0; i < si->pathlen && g->len < g->cap - 1; i++)
      g->buf[g->len++] = (i == 0 && si->abstract) ? '@' : si->path[i];
  }
  gen_str(g, "\n");
}

static void
sock_emit(const struct sockinfo *si, void *arg)
{
  struct sockctx *c = arg;

  if (si->type >= 0 && si->type < SOCK_NTYPES)
    c->counts[si->type]++;
  if (si->type != c->want)
    return;
  gen_str(c->g, "  ");
  gen_num(c->g, c->line++);
  gen_str(c->g, ": ");
  gen_addr_le(c->g, si->lip);
  gen_str(c->g, ":");
  gen_hex(c->g, si->lport, 4);
  gen_str(c->g, " ");
  gen_addr_le(c->g, si->rip);
  gen_str(c->g, ":");
  gen_hex(c->g, si->rport, 4);
  gen_str(c->g, " ");
  gen_hex(c->g, si->state >= 0 ? linux_tcp_state(si->state) : 7, 2);
  gen_str(c->g, " ");
  gen_hex(c->g, si->txbytes, 8);
  gen_str(c->g, ":");
  gen_hex(c->g, si->type == SOCK_DGRAM ? (uint32)si->rxcount : si->rxbytes, 8);
  gen_str(c->g, " 00:00000000 00000000     0        0 ");
  gen_num(c->g, si->index);
  gen_str(c->g, "\n");
}

// how many leading ones a mask has.
static int
prefix_bits(uint32 mask)
{
  int n = 0;

  for (uint32 m = mask; m & 0x80000000; m <<= 1)
    n++;
  return n;
}

// one line of /proc/net/dev, in the columns Linux prints.
static void
dev_emit(const struct netif *d, void *arg)
{
  struct gen *g = arg;

  for (int k = strlen(d->name); k < 6; k++)
    gen_str(g, " ");
  gen_str(g, d->name);
  gen_str(g, ": 0 ");
  gen_num(g, d->rx_packets);
  gen_str(g, " 0 ");
  gen_num(g, d->rx_dropped);
  gen_str(g, " 0 0 0 0 0 ");
  gen_num(g, d->tx_packets);
  gen_str(g, " 0 ");
  gen_num(g, d->tx_dropped);
  gen_str(g, " 0 0 0 0\n");
}

// one line of /proc/net/route: addresses little-endian hexadecimal, as
// the kernel that invented the file happened to write them.
static void
route_emit(const struct route *r, void *arg)
{
  struct gen *g = arg;

  gen_str(g, r->dev->name);
  gen_str(g, "\t");
  gen_addr_le(g, r->dst);
  gen_str(g, "\t");
  gen_addr_le(g, r->gw);
  gen_str(g, r->gw ? "\t0003\t0\t0\t" : "\t0001\t0\t0\t");
  gen_num(g, r->metric);
  gen_str(g, "\t");
  gen_addr_le(g, r->mask);
  gen_str(g, "\t0\t0\t0\n");
}

// one conversation, in the shape conntrack has always printed them:
// the protocol twice (by name and by number), how long this one has
// left, the state if it has one, and the five numbers.
static void
ct_emit(const struct ct *c, void *arg)
{
  struct gen *g = arg;
  static const char *tcpstate[] = {"NONE", "SYN_SENT", "ESTABLISHED",
                                   "CLOSE_WAIT", "CLOSE"};

  gen_str(g, "ipv4     2 ");
  gen_str(g, c->proto == IPPROTO_TCP ? "tcp      6 " :
             c->proto == IPPROTO_UDP ? "udp     17 " :
             c->proto == IPPROTO_ICMP ? "icmp     1 " : "unknown  0 ");
  gen_num(g, c->expires);
  gen_str(g, " ");
  if (c->proto == IPPROTO_TCP) {
    gen_str(g, tcpstate[c->state <= CT_CLOSED ? c->state : 0]);
    gen_str(g, " ");
  }
  gen_str(g, "src=");
  gen_dotted(g, c->src);
  gen_str(g, " dst=");
  gen_dotted(g, c->dst);
  if (c->proto == IPPROTO_ICMP) {
    // an echo has no ports: what identifies it is its type and the
    // identifier its reply carries back
    gen_str(g, " type=");
    gen_num(g, c->icmp_type);
    gen_str(g, " code=0 id=");
    gen_num(g, c->sport);
  } else {
    gen_str(g, " sport=");
    gen_num(g, c->sport);
    gen_str(g, " dport=");
    gen_num(g, c->dport);
  }
  gen_str(g, " packets=");
  gen_num(g, c->packets);
  gen_str(g, " replies=");
  gen_num(g, c->replies);
  if (c->replies)
    gen_str(g, " [ASSURED]");
  if (c->published)
    gen_str(g, " [PUBLISHED]");
  gen_str(g, "\n");
}

// one gateway: the two sides of it, and what it has translated.
static void
gw_emit(const struct cgw *w, void *arg)
{
  struct gen *g = arg;

  gen_str(g, w->name);
  gen_str(g, "\t");
  gen_dotted(g, w->addr);
  gen_str(g, "/");
  gen_num(g, prefix_bits(w->mask));
  gen_str(g, "\tpublic ");
  gen_dotted(g, w->pub_ip);
  gen_str(g, "\tsnat ");
  gen_num(g, w->snat);
  gen_str(g, " dnat ");
  gen_num(g, w->dnat);
  gen_str(g, " flows ");
  gen_num(g, w->translated);
  gen_str(g, " dropped ");
  gen_num(g, w->dropped);
  gen_str(g, "\n");
}

static void
gen_net(struct gen *g, uint inum)
{
  switch (inum) {
  case PINO_NET_DEV:
    gen_str(g, "Inter-|   Receive                            "
               "                    |  Transmit\n");
    gen_str(g, " face |bytes    packets errs drop fifo frame compressed "
               "multicast|bytes    packets errs drop fifo colls carrier "
               "compressed\n");
    // bytes are not counted per interface here, so the columns that
    // hold them are zero rather than a number that is not true
    netif_walk(netns_cur(), dev_emit, g);
    break;

  case PINO_NET_ARP: {
    struct arpctx c = {g};
    gen_str(g, "IP address       HW type     Flags       HW address"
               "            Mask     Device\n");
    acquire(&netlock);
    arp_walk(arp_emit, &c);
    release(&netlock);
    break;
  }

  case PINO_NET_CONNTRACK:
    ct_walk(ct_emit, g);
    break;

  case PINO_NET_GATEWAYS:
    gen_str(g, "Name\tInside\t\tPublic\t\tTranslated\n");
    cgw_walk(gw_emit, g);
    break;

  case PINO_NET_ROUTE:
    gen_str(g, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\t"
               "Mask\t\tMTU\tWindow\tIRTT\n");
    route_walk(netns_cur(), route_emit, g);
    break;

  case PINO_NET_TCP:
  case PINO_NET_UDP: {
    struct sockctx c;
    memset(&c, 0, sizeof(c));
    c.g = g;
    c.want = inum == PINO_NET_TCP ? SOCK_STREAM : SOCK_DGRAM;
    gen_str(g, "  sl  local_address rem_address   st tx_queue:rx_queue "
               "tr:tm->when retrnsmt   uid  timeout inode\n");
    sock_walk(sock_emit, &c);
    break;
  }

  case PINO_NET_UNIX:
    gen_str(g, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    unixsock_walk(unix_emit, g);
    break;

  case PINO_NET_SOCKSTAT: {
    struct sockctx c;
    memset(&c, 0, sizeof(c));
    c.g = g;
    c.want = -1;
    sock_walk(sock_emit, &c);
    gen_str(g, "sockets: used ");
    gen_num(g, c.counts[SOCK_DGRAM] + c.counts[SOCK_STREAM] + c.counts[SOCK_ICMP]);
    gen_str(g, "\nTCP: inuse ");
    gen_num(g, c.counts[SOCK_STREAM]);
    gen_str(g, "\nUDP: inuse ");
    gen_num(g, c.counts[SOCK_DGRAM]);
    gen_str(g, "\nRAW: inuse ");
    gen_num(g, c.counts[SOCK_ICMP]);
    gen_str(g, "\n");
    break;
  }
  }
}

static void
gen_top(struct gen *g, uint inum)
{
  switch (inum) {
  case PINO_MOUNTS:
    vfs_list(mounts_emit, g);
    break;

  case PINO_MEMINFO: {
    struct meminfo mi;
    // a kernel stack that has been released sits in quarantine until
    // the harts that might still have it in a TLB have been told;
    // draining first means this file and sysconf() agree about how
    // much memory there is, rather than differing by a few stacks.
    kstack_drain();
    kmem_info(&mi);
    gen_kv(g, "MemTotal", mi.total_pages * 4, "kB");
    gen_kv(g, "MemFree", mi.free_pages * 4, "kB");
    gen_kv(g, "MemAvailable", (mi.free_pages + blk_reclaimable()) * 4, "kB");
    uint64 hits, misses, dirty;
    blk_cache_stats(&hits, &misses, &dirty);
    // what the block cache holds, and how much of it kalloc would take
    // back if a page were wanted: Linux calls the second "available".
    gen_kv(g, "Cached", blk_cache_pages() * 4, "kB");
    gen_kv(g, "MemReclaimable", blk_reclaimable() * 4, "kB");
    gen_kv(g, "Dirty", dirty * 4, "kB");
    // the stacks of the tasks that exist, as Linux reports them: a
    // slot is in use from the moment a task is made until the pages
    // under it are given back, which is where a fork storm that has
    // not finished winding down shows up.
    uint64 ksused, ksslots;
    kstack_stats(&ksused, &ksslots);
    gen_kv(g, "KernelStack", ksused * KSTACK_PAGES * 4, "kB");
    gen_kv(g, "PageTables", mi.db_pages * 4, "kB");
    gen_kv(g, "Reserved", mi.reserve_pages * 4, "kB");
    break;
  }

  case PINO_CPUINFO:
    for (int i = 0; i < plat.ncpu; i++) {
      gen_str(g, "processor\t: ");
      gen_num(g, i);
      gen_str(g, "\nhart\t\t: ");
      gen_num(g, i);
      gen_str(g, "\nisa\t\t: rv64imafdc");
      gen_str(g, plat.has_h ? "h\n" : "\n");
      gen_str(g, "mmu\t\t: sv");
      gen_num(g, plat.mmu_levels == 4 ? 48 : 39);
      gen_str(g, "\n\n");
    }
    break;

  case PINO_UPTIME: {
    uint64 ns = clock_monotonic_ns();
    uint64 hundredths = (ns / 10000000UL) % 100;
    gen_num(g, ns / 1000000000UL);
    gen_str(g, ".");
    // both digits, always: a reader that subtracts two of these gets
    // nonsense from `3.7' when what was meant was three and seven
    // hundredths
    if (hundredths < 10)
      gen_str(g, "0");
    gen_num(g, hundredths);
    gen_str(g, " 0.00\n");
    break;
  }

  case PINO_SCHEDSTAT: {
    // one line per hart: runnable tasks, their weights summed,
    // context switches, tasks taken from a busier hart
    uint64 st[NCPU * SCHED_STAT_FIELDS];
    memset(st, 0, sizeof(st));
    sched_stats(st, NCPU * SCHED_STAT_FIELDS);
    gen_str(g, "cpu nr_running load switches steals tick resched preempts\n");
    for (int i = 0; i < plat.ncpu; i++) {
      gen_num(g, i);
      for (int j = 0; j < SCHED_STAT_FIELDS; j++) {
        gen_str(g, " ");
        gen_num(g, st[i * SCHED_STAT_FIELDS + j]);
      }
      gen_str(g, "\n");
    }
    // and one line per task that is runnable or running: where it is,
    // what it weighs, and how much virtual time it has. Reading this
    // while three tasks of different nice are spinning is how the
    // sharing rule is checked from outside the kernel.
    // and one line per group: what a VM, a container or a cgroup has
    // been given and what it has done with it
    {
      uint64 gs[NSCHEDGROUP * 9];
      char names[NSCHEDGROUP][SCHED_GROUP_NAMELEN];
      int ng = sched_group_list(gs, NSCHEDGROUP, names);
      if (ng > 0)
        gen_str(g, "group id name tasks weight quota_us period_us throttles frozen runnable usage_us\n");
      for (int i = 0; i < ng; i++) {
        uint64 *o = &gs[i * 9];
        gen_str(g, "group ");
        gen_num(g, o[0]);
        gen_str(g, " ");
        names[i][SCHED_GROUP_NAMELEN - 1] = 0;
        gen_str(g, names[i][0] ? names[i] : "-");
        for (int k = 1; k < 9; k++) {
          gen_str(g, " ");
          gen_num(g, o[k]);
        }
        gen_str(g, "\n");
      }
    }
    gen_str(g, "task pid cpu state nice weight vruntime exec\n");
    proc_iter_start();
    for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
      if (p->state != RUNNABLE && p->state != RUNNING)
        continue;
      gen_str(g, "task ");
      gen_num(g, p->pid);
      gen_str(g, " ");
      gen_num(g, p->cpu < 0 ? 0 : p->cpu);
      gen_str(g, p->state == RUNNING ? " run " : " rdy ");
      if (p->nice < 0) {
        gen_str(g, "-");
        gen_num(g, (uint64)(-p->nice));
      } else
        gen_num(g, p->nice);
      gen_str(g, " ");
      gen_num(g, p->se.weight);
      gen_str(g, " ");
      gen_num(g, p->se.vruntime);
      gen_str(g, " ");
      gen_num(g, p->sum_exec);
      gen_str(g, "\n");
    }
    proc_iter_end();
    break;
  }

  case PINO_AUDIT: {
    // the record of privileged operations. Who became whom, and when,
    // is not for everybody to read: an unprivileged reader gets the
    // header and nothing else. What fits is the newest part of the
    // ring -- this file is one page, and the ring is larger.
    uint64 seq, lost;
    audit_stats(&seq, &lost);
    gen_str(g, "# audit: ");
    gen_num(g, seq);
    gen_str(g, " records, ");
    gen_num(g, lost);
    gen_str(g, " dropped\n");
    if (!cap_ok(CAP_SYS_ADMIN) || !userns_in_root())
      break;
    g->len += audit_snapshot(g->buf + g->len, g->cap - g->len);
    break;
  }

  case PINO_VMM: {
    // the machines this host is running: who owns each one, what state
    // it is in, and how many descriptors name it. A machine is its
    // owner's business, so a reader who is not privileged sees only
    // their own -- the registry is not a list of who is running what.
    // a page rather than a static array: two readers of this file at
    // once would otherwise fill the same one, and NVM entries are too
    // much to put on a kernel stack.
    struct vmentry *vms = (struct vmentry *)kalloc();
    if (vms == 0)
      break;
    int n = vm_list(vms, PGSIZE / (int)sizeof(struct vmentry));
    static const char *const st[] = {"free", "new", "running", "halting",
                                     "paused"};
    static const char *const kd[] = {"plic", "aplic", "imsic"};
    int all = cap_ok(CAP_SYS_ADMIN) && userns_in_root();
    uint me = myproc()->cred.euid;
    gen_str(g, "id name owner state intc vcpus mem_mb resident_mb fds\n");
    for (int i = 0; i < n; i++) {
      if (!all && vms[i].owner != me)
        continue;
      vms[i].name[VM_NAMELEN - 1] = 0;
      gen_num(g, vms[i].id);
      gen_str(g, " ");
      gen_str(g, vms[i].name[0] ? vms[i].name : "-");
      gen_str(g, " ");
      gen_num(g, vms[i].owner);
      gen_str(g, " ");
      gen_str(g, st[vms[i].state <= VM_PAUSED ? vms[i].state : 0]);
      gen_str(g, " ");
      gen_str(g, kd[vms[i].kind >= 0 && vms[i].kind <= 2 ? vms[i].kind : 0]);
      gen_str(g, " ");
      gen_num(g, vms[i].nvcpu);
      gen_str(g, " ");
      gen_num(g, vms[i].mem_mb);
      gen_str(g, " ");
      gen_num(g, vms[i].resident_mb);
      gen_str(g, " ");
      gen_num(g, vms[i].fdref);
      gen_str(g, "\n");
    }
    kfree(vms);
    break;
  }

  case PINO_SLABINFO: {
    // one line per object cache: what it holds, how big, how many fit
    // in a slab, how many slabs it owns, how many objects are out, and
    // the pages that comes to. A cache that has grown keeps a slab
    // even when it is empty, which is why a run of anything leaves the
    // kernel holding a few pages more than it started with.
    uint64 cs[MAXCACHES * 5];
    char names[MAXCACHES][16];
    int n = kmem_cache_list(cs, MAXCACHES, names);
    gen_str(g, "name objsize per_slab slabs objects pages\n");
    for (int i = 0; i < n; i++) {
      names[i][15] = 0;
      gen_str(g, names[i]);
      for (int k = 0; k < 5; k++) {
        gen_str(g, " ");
        gen_num(g, cs[i * 5 + k]);
      }
      gen_str(g, "\n");
    }
    break;
  }

  case PINO_VERSION:
    gen_str(g, "xv6H (riscv64) hypervisor and operating system\n");
    break;

  case PINO_FILESYSTEMS:
    gen_str(g, "\text4\n\tprocfs\n\txv6\n");
    break;

  case PINO_PARTITIONS:
    gen_str(g, "major minor  #blocks  name\n\n");
    for (int i = 0; i < MAXDISK; i++) {
      struct gendisk *d = blk_by_index(i);
      if (d == 0)
        continue;
      gen_str(g, "   8 ");
      gen_num(g, i);
      gen_str(g, " ");
      gen_num(g, d->nsectors / 2);
      gen_str(g, " ");
      gen_str(g, d->name);
      gen_str(g, "\n");
    }
    break;

  case PINO_DISKSTATS:
    for (int i = 0; i < MAXDISK; i++) {
      struct gendisk *d = blk_by_index(i);
      if (d == 0)
        continue;
      gen_str(g, d->name);
      gen_str(g, " reads ");
      gen_num(g, d->reads);
      gen_str(g, " writes ");
      gen_num(g, d->writes);
      gen_str(g, "\n");
    }
    break;

  case PINO_STAT: {
    gen_str(g, "processes ");
    gen_num(g, proc_count());
    gen_str(g, "\nbtime 0\n");
    break;
  }

  case PINO_SELF:
    gen_num(g, pid_in_ns(myproc(), pidns_current()));
    gen_str(g, "\n");
    break;

  case PINO_NET_DEV:
  case PINO_NET_ARP:
  case PINO_NET_ROUTE:
  case PINO_NET_TCP:
  case PINO_NET_UDP:
  case PINO_NET_SOCKSTAT:
  case PINO_NET_UNIX:
  case PINO_NET_CONNTRACK:
  case PINO_NET_GATEWAYS:
    gen_net(g, inum);
    break;

  default:
    break;
  }
}

// one process's file
static void
gen_pid(struct gen *g, int pid, int which)
{
  struct proc *p = proc_get_ns(pidns_current(), pid);

  if (p == 0)
    return;
  switch (which) {
  case PPID_STATUS: {
    struct pidns *pns = pidns_current();
    gen_str(g, "Name:\t");
    gen_str(g, p->name);
    gen_str(g, "\nPid:\t");
    gen_num(g, pid_in_ns(p, pns));
    gen_str(g, "\nPPid:\t");
    // a parent outside the namespace cannot be named in it
    gen_num(g, p->parent ? pid_in_ns(p->parent, pns) : 0);
    gen_str(g, "\nState:\t");
    switch (p->state) {
    case RUNNING: gen_str(g, "R (running)"); break;
    case RUNNABLE: gen_str(g, "R (runnable)"); break;
    case SLEEPING: gen_str(g, "S (sleeping)"); break;
    case ZOMBIE: gen_str(g, "Z (zombie)"); break;
    default: gen_str(g, "? (unknown)"); break;
    }
    gen_str(g, "\nThreads:\t");
    gen_num(g, proc_nthreads(p));
    gen_str(g, "\nUid:\t");
    gen_num(g, p->cred.uid);
    gen_str(g, "\t");
    gen_num(g, p->cred.euid);
    gen_str(g, "\nGid:\t");
    gen_num(g, p->cred.gid);
    gen_str(g, "\t");
    gen_num(g, p->cred.egid);
    gen_str(g, "\nVmSize:\t");
    gen_num(g, p->mm ? p->mm->pages * 4 : 0);
    gen_str(g, " kB\nTty:\t");
    gen_num(g, p->tty);
    gen_str(g, "\nUtime:\t");
    gen_num(g, p->utime);
    gen_str(g, "\nStime:\t");
    gen_num(g, p->stime);
    gen_str(g, "\n");
    break;
  }

  case PPID_CMDLINE:
    gen_str(g, p->name);
    break;

  case PPID_STAT:
    gen_num(g, pid_in_ns(p, pidns_current()));
    gen_str(g, " (");
    gen_str(g, p->name);
    gen_str(g, ") ");
    gen_str(g, p->state == RUNNING     ? "R "
                : p->state == SLEEPING ? "S "
                : p->state == ZOMBIE   ? "Z "
                                       : "R ");
    gen_num(g, p->parent ? pid_in_ns(p->parent, pidns_current()) : 0);
    gen_str(g, " ");
    gen_num(g, p->pgid);
    gen_str(g, " ");
    gen_num(g, p->sid);
    gen_str(g, " ");
    gen_num(g, p->tty);
    gen_str(g, " ");
    gen_num(g, p->utime);
    gen_str(g, " ");
    gen_num(g, p->stime);
    gen_str(g, " ");
    gen_num(g, p->mm ? p->mm->pages : 0);
    gen_str(g, "\n");
    break;

  case PPID_UIDMAP:
  case PPID_GIDMAP: {
    // "inside host count", as Linux writes it. A namespace with no
    // mapping yet has an empty file.
    struct nsproxy *nsp = p->ns;
    struct userns *un = nsp ? nsp->user : 0;
    unsigned int inside, host, count;
    if (un == 0 || userns_map_of(un, which == PPID_UIDMAP, &inside, &host,
                                 &count) < 0)
      break;
    gen_str(g, "        ");
    gen_num(g, inside);
    gen_str(g, "        ");
    gen_num(g, host);
    gen_str(g, "        ");
    gen_num(g, count);
    gen_str(g, "\n");
    break;
  }

  case PPID_MAPS:
    gen_str(g, "(areas are listed once the address space grows a reader)\n");
    break;

  default:
    break;
  }
  proc_release(p);
}

// ---- inodes ----

static int
is_pid_dir(uint inum, int *pid, int *which)
{
  if (inum < PINO_FIRST_PID)
    return 0;
  uint v = inum - PINO_FIRST_PID;
  *pid = v >> 4;
  *which = v & 0xf;
  return 1;
}

static uint
pid_inum(int pid, int which)
{
  return PINO_FIRST_PID + ((uint)pid << 4) + which;
}

// what a descriptor points at, written the way /proc has always
// written the things that never had a name. this kernel keeps no path
// for an open file -- there is no dentry cache -- so a file is named
// by the numbers that identify it, which is what a program comparing
// two descriptors actually wants.
static void
name_file(struct file *f, char *out, int size)
{
  struct gen g = {out, 0, size};

  out[0] = 0;
  if (f == 0)
    return;
  switch (f->type) {
  case FD_INODE:
    if (f->ip) {
      gen_str(&g, f->ip->sb && f->ip->sb->type[0] ? f->ip->sb->type : "inode");
      gen_str(&g, ":[");
      gen_num(&g, f->ip->dev);
      gen_str(&g, ":");
      gen_num(&g, f->ip->inum);
      gen_str(&g, "]");
      break;
    }
    gen_str(&g, "inode:[0]");
    break;
  case FD_DEVICE:
    gen_str(&g, "dev:[");
    gen_num(&g, f->ip ? (uint64)f->ip->major : 0);
    gen_str(&g, ":");
    gen_num(&g, f->ip ? (uint64)f->ip->minor : 0);
    gen_str(&g, "]");
    break;
  case FD_BLOCK:
    gen_str(&g, "block:[");
    gen_str(&g, f->disk && f->disk->name[0] ? f->disk->name : "?");
    gen_str(&g, "]");
    break;
  case FD_PIPE:
    // the pointer makes a number that is the same for both ends of
    // one pipe and different for any other, which is all an id is for
    gen_str(&g, "pipe:[");
    gen_num(&g, ((uint64)f->pipe) >> 4);
    gen_str(&g, "]");
    break;
  case FD_SOCK:
    gen_str(&g, "socket:[");
    gen_num(&g, ((uint64)f->sock) >> 4);
    gen_str(&g, "]");
    break;
  case FD_UNIX:
    gen_str(&g, "socket:[");
    gen_num(&g, ((uint64)f->un) >> 4);
    gen_str(&g, "]");
    break;
  default:
    gen_str(&g, "unknown:[0]");
    break;
  }
  g.buf[g.len] = 0;
}

// the same, for a directory this process is sitting in or rooted at
static void
name_inode(struct inode *ip, char *out, int size)
{
  struct gen g = {out, 0, size};

  if (ip == 0) {
    gen_str(&g, "unknown:[0]");
  } else {
    gen_str(&g, ip->sb && ip->sb->type[0] ? ip->sb->type : "inode");
    gen_str(&g, ":[");
    gen_num(&g, ip->dev);
    gen_str(&g, ":");
    gen_num(&g, ip->inum);
    gen_str(&g, "]");
  }
  g.buf[g.len] = 0;
}

// the descriptor, with a reference held so it cannot be closed and
// freed while it is being read
static struct file *
grab_fd(int pid, int fd, struct proc **held)
{
  struct proc *p = proc_get_ns(pidns_current(), pid);
  struct file *f = 0;

  *held = 0;
  if (p == 0)
    return 0;
  if (p->fdt) {
    acquire(&p->fdt->lock);
    if (fd >= 0 && fd < p->fdt->n && p->fdt->ofile[fd])
      f = filedup(p->fdt->ofile[fd]);
    release(&p->fdt->lock);
  }
  proc_release(p);
  return f;
}

static void
procfs_load(struct inode *ip)
{
  int pid, which;

  ip->nlink = 1;
  // nothing here is stored, so the mode is the one every file of
  // this file system has: readable by all, written by root alone.
  ip->mode = 0444;
  ip->uid = ip->gid = 0;
  ip->major = 0;
  ip->minor = 0;
  ip->size = 0;
  int fdnum;

  if (is_fd_inum(ip->inum, &pid, &fdnum)) {
    // a descriptor is a link to whatever it holds, and it is the
    // owner's business alone
    ip->type = T_SYMLINK;
    ip->mode = 0500;
    struct proc *p = proc_get_ns(pidns_current(), pid);
    if (p) {
      ip->uid = p->cred.euid;
      ip->gid = p->cred.egid;
      proc_release(p);
    }
    ip->valid = 1;
    return;
  }
  if (ip->inum == PINO_SELF) {
    // a link to the caller's own directory, which is what makes
    // /proc/self/fd and the rest work for whoever asks
    ip->type = T_SYMLINK;
    ip->mode = 0777;
    ip->valid = 1;
    return;
  }
  int nskind;
  if (is_ns_inum(ip->inum, &pid, &nskind)) {
    ip->type = T_FILE;
    ip->mode = 0444;
    struct proc *pp = proc_get_ns(pidns_current(), pid);
    if (pp) {
      ip->uid = pp->cred.euid;
      ip->gid = pp->cred.egid;
      proc_release(pp);
    }
    ip->valid = 1;
    return;
  }
  if (ip->inum == PINO_ROOT || ip->inum == PINO_NET) {
    ip->type = T_DIR;
    ip->mode = 0555;
    ip->nlink = 2;
  } else if (is_pid_dir(ip->inum, &pid, &which)) {
    ip->type = which == PPID_DIR ? T_DIR : T_FILE;
    if (which == PPID_DIR)
      ip->mode = 0555;
    if (which == PPID_FD) {
      ip->type = T_DIR;
      ip->mode = 0500;   // what a process holds open is its own affair
      ip->nlink = 2;
    }
    if (which == PPID_UIDMAP || which == PPID_GIDMAP) {
      // the one writable thing under a process: who its user namespace
      // calls whom. Written once, by somebody the namespace above
      // trusts.
      ip->mode = 0644;
    }
    if (which == PPID_NS) {
      ip->type = T_DIR;
      ip->mode = 0555;   // which namespaces a process is in is not a secret
      ip->nlink = 2;
    }
    if (which == PPID_CWD || which == PPID_ROOT) {
      ip->type = T_SYMLINK;
      ip->mode = 0500;
    }
    // a process's directory belongs to whoever runs it, and what is in
    // it is theirs: the environment of another user's process is not
    // something to hand out, and Linux does not either.
    struct proc *p = proc_get_ns(pidns_current(), pid);
    if (p) {
      ip->uid = p->cred.euid;
      ip->gid = p->cred.egid;
      proc_release(p);
    }
    if (which == PPID_CMDLINE)
      ip->mode = 0444; // what a process was started with is not a secret
  } else {
    ip->type = T_FILE;
  }
  ip->valid = 1;
}

// what /proc/<pid>/ns/<kind> names, for setns(2): the process and the
// kind, from which the kernel finds the namespace. -1 if this inode is
// not one of those files.
int
procfs_ns_of(struct inode *ip, int *pid, int *kind)
{
  if (ip == 0 || ip->sb == 0 || strncmp(ip->sb->type, "procfs", 7) != 0)
    return -1;
  return is_ns_inum(ip->inum, pid, kind) ? 0 : -1;
}

static int
procfs_read(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  char *page = kalloc();
  int pid, which;

  if (page == 0)
    return -ENOMEM;
  struct gen g = {page, 0, PGSIZE};
  int nskind;
  if (is_ns_inum(ip->inum, &pid, &nskind)) {
    // "uts:[3]": the kind and which one of it, so that two processes in
    // the same namespace read the same thing and two in different ones
    // do not. That is the whole of what this file is for, besides being
    // something setns(2) can be given a descriptor for.
    gen_str(&g, ns_kind_name(nskind));
    gen_str(&g, ":[");
    gen_num(&g, ns_id_of(pid, nskind));
    gen_str(&g, "]\n");
  } else if (is_pid_dir(ip->inum, &pid, &which)) {
    gen_pid(&g, pid, which);
  } else {
    gen_top(&g, ip->inum);
  }
  g.buf[g.len] = 0;

  int avail = g.len - (int)off;
  if (avail <= 0) {
    kfree(page);
    return 0;
  }
  if ((int)n > avail)
    n = avail;
  if (either_copyout(user_dst, dst, page + off, n) < 0) {
    kfree(page);
    return -EFAULT;
  }
  kfree(page);
  return n;
}

static struct inode *
procfs_iget(uint inum)
{
  struct inode *ip = iget(PROCFS_DEV, inum);

  if (ip) {
    ip->sb = proc_sb;
    ip->ops = &procfs_iops;
  }
  return ip;
}

static struct inode *
procfs_lookup(struct inode *dp, char *name, uint *poff)
{
  int pid, which;

  if (poff)
    *poff = 0;
  if (dp->inum == PINO_ROOT) {
    if (strncmp(name, ".", 2) == 0)
      return procfs_iget(PINO_ROOT);
    if (strncmp(name, "..", 3) == 0)
      return procfs_iget(PINO_ROOT);
    for (int i = 0; topfiles[i].name; i++)
      if (strncmp(name, topfiles[i].name, DIRSIZ) == 0)
        return procfs_iget(topfiles[i].inum);
    // a number is a process, if it is one
    int n = 0;
    for (char *p = name; *p; p++) {
      if (*p < '0' || *p > '9')
        return 0;
      n = n * 10 + (*p - '0');
    }
    struct proc *p = proc_get_ns(pidns_current(), n);
    if (p == 0)
      return 0;
    proc_release(p);
    return procfs_iget(pid_inum(n, PPID_DIR));
  }
  if (dp->inum == PINO_NET) {
    if (strncmp(name, ".", 2) == 0)
      return procfs_iget(PINO_NET);
    if (strncmp(name, "..", 3) == 0)
      return procfs_iget(PINO_ROOT);
    for (int i = 0; netfiles[i].name; i++)
      if (strncmp(name, netfiles[i].name, DIRSIZ) == 0)
        return procfs_iget(netfiles[i].inum);
    return 0;
  }
  if (is_pid_dir(dp->inum, &pid, &which) && which == PPID_DIR) {
    if (strncmp(name, ".", 2) == 0)
      return procfs_iget(dp->inum);
    if (strncmp(name, "..", 3) == 0)
      return procfs_iget(PINO_ROOT);
    for (int i = 1; i < PPID_NFILES; i++)
      if (pid_files[i] && strncmp(name, pid_files[i], DIRSIZ) == 0)
        return procfs_iget(pid_inum(pid, i));
    return 0;
  }
  if (is_pid_dir(dp->inum, &pid, &which) && which == PPID_NS) {
    if (strncmp(name, ".", 2) == 0)
      return procfs_iget(dp->inum);
    if (strncmp(name, "..", 3) == 0)
      return procfs_iget(pid_inum(pid, PPID_DIR));
    for (int k = 0; k < NS_KINDS; k++)
      if (ns_kind_name(k) && strncmp(name, ns_kind_name(k), DIRSIZ) == 0)
        return procfs_iget(ns_inum(pid, k));
    return 0;
  }
  if (is_pid_dir(dp->inum, &pid, &which) && which == PPID_FD) {
    if (strncmp(name, ".", 2) == 0)
      return procfs_iget(dp->inum);
    if (strncmp(name, "..", 3) == 0)
      return procfs_iget(pid_inum(pid, PPID_DIR));
    int fd = 0;
    for (char *p = name; *p; p++) {
      if (*p < '0' || *p > '9')
        return 0;
      fd = fd * 10 + (*p - '0');
    }
    // only a descriptor that is really open has an entry
    struct proc *held;
    struct file *f = grab_fd(pid, fd, &held);
    if (f == 0)
      return 0;
    fileclose(f);
    return procfs_iget(fd_inum(pid, fd));
  }
  return 0;
}

static int
procfs_readdir(struct inode *dp, uint64 *pos, char *name, int namemax, uint *ino, uchar *type)
{
  int pid, which;

  if (dp->inum == PINO_ROOT) {
    uint64 i = *pos;
    if (i == 0) {
      safestrcpy(name, ".", namemax);
      *ino = PINO_ROOT;
      *type = 4;
      (*pos)++;
      return 1;
    }
    if (i == 1) {
      safestrcpy(name, "..", namemax);
      *ino = PINO_ROOT;
      *type = 4;
      (*pos)++;
      return 1;
    }
    i -= 2;
    int ntop = 0;
    while (topfiles[ntop].name)
      ntop++;
    if (i < (uint64)ntop) {
      safestrcpy(name, topfiles[i].name, namemax);
      *ino = topfiles[i].inum;
      *type = topfiles[i].inum == PINO_NET  ? 4
              : topfiles[i].inum == PINO_SELF ? 10 : 8;
      (*pos)++;
      return 1;
    }
    // then one directory per process -- one per process the caller can
    // see, numbered the way the caller numbers it. Inside a container
    // that is the container's processes and nothing else, which is what
    // makes `ps' inside one honest.
    uint64 want = i - ntop;
    uint64 seen = 0;
    struct pidns *pns = pidns_current();
    proc_iter_start();
    for (struct proc *p = proc_iter_next(0); p; p = proc_iter_next(p)) {
      if (p->state == UNUSED)
        continue;
      if (!pid_visible(p, pns))
        continue;
      if (seen == want) {
        int pidv = pid_in_ns(p, pns);
        proc_iter_end();
        // the name is the number
        char tmp[16];
        int n = 0;
        int v = pidv;
        if (v == 0)
          tmp[n++] = '0';
        while (v) {
          tmp[n++] = '0' + v % 10;
          v /= 10;
        }
        int k = 0;
        while (n > 0 && k < namemax - 1)
          name[k++] = tmp[--n];
        name[k] = 0;
        *ino = pid_inum(pidv, PPID_DIR);
        *type = 4;
        (*pos)++;
        return 1;
      }
      seen++;
    }
    proc_iter_end();
    return 0;
  }

  if (dp->inum == PINO_NET) {
    uint64 i = *pos;
    if (i == 0) {
      safestrcpy(name, ".", namemax);
      *ino = PINO_NET;
      *type = 4;
      (*pos)++;
      return 1;
    }
    if (i == 1) {
      safestrcpy(name, "..", namemax);
      *ino = PINO_ROOT;
      *type = 4;
      (*pos)++;
      return 1;
    }
    i -= 2;
    int n = 0;
    while (netfiles[n].name)
      n++;
    if (i < (uint64)n) {
      safestrcpy(name, netfiles[i].name, namemax);
      *ino = netfiles[i].inum;
      *type = 8;
      (*pos)++;
      return 1;
    }
    return 0;
  }

  if (is_pid_dir(dp->inum, &pid, &which) && which == PPID_NS) {
    uint64 i = *pos;
    if (i == 0) {
      safestrcpy(name, ".", namemax);
      *ino = dp->inum;
      *type = 4;
      (*pos)++;
      return 1;
    }
    if (i == 1) {
      safestrcpy(name, "..", namemax);
      *ino = pid_inum(pid, PPID_DIR);
      *type = 4;
      (*pos)++;
      return 1;
    }
    for (uint64 k = i - 2; k < NS_KINDS; k++) {
      if (ns_kind_name((int)k) == 0)
        continue;
      safestrcpy(name, ns_kind_name((int)k), namemax);
      *ino = ns_inum(pid, (int)k);
      *type = 8;
      *pos = k + 3;
      return 1;
    }
    return 0;
  }

  if (is_pid_dir(dp->inum, &pid, &which) && which == PPID_FD) {
    uint64 i = *pos;
    if (i == 0) {
      safestrcpy(name, ".", namemax);
      *ino = dp->inum;
      *type = 4;
      (*pos)++;
      return 1;
    }
    if (i == 1) {
      safestrcpy(name, "..", namemax);
      *ino = pid_inum(pid, PPID_DIR);
      *type = 4;
      (*pos)++;
      return 1;
    }
    // one entry per open descriptor, walked from where the last call
    // left off rather than counted from the start
    struct proc *p = proc_get_ns(pidns_current(), pid);
    if (p == 0 || p->fdt == 0) {
      if (p)
        proc_release(p);
      return 0;
    }
    int found = -1;
    acquire(&p->fdt->lock);
    for (int fd = (int)(i - 2); fd < p->fdt->n; fd++)
      if (p->fdt->ofile[fd]) {
        found = fd;
        break;
      }
    release(&p->fdt->lock);
    proc_release(p);
    if (found < 0)
      return 0;
    char tmp[16];
    int n = 0, v = found;
    if (v == 0)
      tmp[n++] = '0';
    while (v) {
      tmp[n++] = '0' + v % 10;
      v /= 10;
    }
    int k = 0;
    while (n > 0 && k < namemax - 1)
      name[k++] = tmp[--n];
    name[k] = 0;
    *ino = fd_inum(pid, found);
    *type = 10;   // a symbolic link
    *pos = (uint64)found + 3;
    return 1;
  }

  if (is_pid_dir(dp->inum, &pid, &which) && which == PPID_DIR) {
    uint64 i = *pos;
    if (i == 0) {
      safestrcpy(name, ".", namemax);
      *ino = dp->inum;
      *type = 4;
      (*pos)++;
      return 1;
    }
    if (i == 1) {
      safestrcpy(name, "..", namemax);
      *ino = PINO_ROOT;
      *type = 4;
      (*pos)++;
      return 1;
    }
    i -= 1; // the files start at index 1 in pid_files
    if (i < PPID_NFILES && pid_files[i]) {
      safestrcpy(name, pid_files[i], namemax);
      *ino = pid_inum(pid, i);
      *type = (i == PPID_FD || i == PPID_NS) ? 4
              : (i == PPID_CWD || i == PPID_ROOT) ? 10 : 8;
      (*pos)++;
      return 1;
    }
    return 0;
  }
  return 0;
}

// where a link under /proc points
static int
procfs_readlink(struct inode *ip, char *buf, int max)
{
  char name[64];
  int pid, which, fdnum;

  name[0] = 0;
  if (ip->inum == PINO_SELF) {
    struct gen g = {name, 0, sizeof(name)};
    gen_num(&g, myproc()->pid);
    g.buf[g.len] = 0;
    int n = strlen(name);
    if (n > max)
      n = max;
    memmove(buf, name, n);
    return n;
  }
  if (is_fd_inum(ip->inum, &pid, &fdnum)) {
    struct proc *held;
    struct file *f = grab_fd(pid, fdnum, &held);
    if (f == 0)
      return -ENOENT;
    name_file(f, name, sizeof(name));
    fileclose(f);
  } else if (is_pid_dir(ip->inum, &pid, &which) &&
             (which == PPID_CWD || which == PPID_ROOT)) {
    struct proc *p = proc_get_ns(pidns_current(), pid);
    if (p == 0)
      return -ENOENT;
    struct inode *dir = 0;
    int is_root_dir = 0;
    if (p->fs) {
      acquire(&p->fs->lock);
      dir = which == PPID_CWD ? p->fs->cwd : p->fs->root;
      release(&p->fs->lock);
    }
    if (which == PPID_ROOT && dir == 0)
      is_root_dir = 1;   // not chrooted: the file system's own root
    if (is_root_dir) {
      struct gen g = {name, 0, sizeof(name)};
      gen_str(&g, "root:[/]");
      g.buf[g.len] = 0;
    } else {
      name_inode(dir, name, sizeof(name));
    }
    proc_release(p);
  } else {
    return -EINVAL;
  }

  int n = strlen(name);
  if (n > max)
    n = max;
  memmove(buf, name, n);
  return n;
}

static int
procfs_write(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  int pid, which;

  (void)off;
  if (!is_pid_dir(ip->inum, &pid, &which))
    return -EACCES;
  if (which != PPID_UIDMAP && which != PPID_GIDMAP)
    return -EACCES;
  if (n == 0 || n > 64)
    return -EINVAL;
  char buf[65];
  if (either_copyin(buf, user_src, src, n) < 0)
    return -EFAULT;
  buf[n] = 0;

  // three numbers: what it is called inside, what it is on the machine,
  // and how many of them.
  unsigned int v[3] = {0, 0, 0};
  int got = 0;
  for (char *q = buf; *q && got < 3;) {
    while (*q == ' ' || *q == '\t' || *q == '\n')
      q++;
    if (*q < '0' || *q > '9')
      break;
    unsigned int x = 0;
    while (*q >= '0' && *q <= '9')
      x = x * 10 + (unsigned int)(*q++ - '0');
    v[got++] = x;
  }
  if (got != 3)
    return -EINVAL;

  struct proc *t = proc_get_ns(pidns_current(), pid);
  if (t == 0)
    return -ESRCH;
  struct nsproxy *nsp = t->ns;
  struct userns *un = nsp ? nsp->user : 0;
  proc_release(t);
  if (un == 0 || un == userns_root())
    return -EPERM;         // the machine's own numbering is not a map
  // writing somebody's map is a privileged thing in the namespace above
  // it: a container that could widen its own mapping would be a
  // container in name only.
  if (!cap_ok(CAP_SETUID) || !userns_in_root())
    return -EPERM;
  int r = userns_set_map(un, which == PPID_UIDMAP, v[0], v[1], v[2]);
  return r < 0 ? r : (int)n;
}

static const struct inode_ops procfs_iops = {
    .load = procfs_load,
    .read = procfs_read,
    .write = procfs_write,
    .lookup = procfs_lookup,
    .readdir = procfs_readdir,
    .readlink = procfs_readlink,
};

static struct inode *
procfs_sb_iget(struct super *sb, uint inum)
{
  (void)sb;
  return procfs_iget(inum);
}

static void
procfs_statfs(struct super *sb, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
              uint64 *bsize, uint64 *namelen)
{
  (void)sb;
  *blocks = *bfree = 0;
  *files = proc_count();
  *ffree = 0;
  *bsize = PGSIZE;
  *namelen = DIRSIZ;
}

static const struct super_ops procfs_sops = {
    .iget = procfs_sb_iget,
    .statfs = procfs_statfs,
};

static struct super *
procfs_mount(struct gendisk *disk, const char *source, int readonly, const char *opts)
{
  (void)disk;
  (void)source;
  (void)readonly;
  (void)opts;

  struct super *sb = vfs_super_alloc();
  if (sb == 0)
    return 0;
  sb->ops = &procfs_sops;
  sb->iops = &procfs_iops;
  proc_sb = sb;
  sb->root = procfs_iget(PINO_ROOT);
  if (sb->root == 0) {
    vfs_super_free(sb);
    return 0;
  }
  return sb;
}

static struct fs_type procfs_type = {
    .name = "procfs",
    .mount = procfs_mount,
};

void
procfs_init(void)
{
  vfs_register(&procfs_type);
}
