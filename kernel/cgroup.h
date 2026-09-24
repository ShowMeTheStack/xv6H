#ifndef XV6_CGROUP_H
#define XV6_CGROUP_H
//
// control groups (kernel/cgroup.c): a name, a place in a tree, and what
// the processes in it may take.
//

#define NCGROUP 16
#define CG_NAMELEN 24

// the files, by number: what cg_get and cg_set_limit are about
#define CG_CPU_WEIGHT     0
#define CG_CPU_MAX        1
#define CG_MEMORY_MAX     2
#define CG_MEMORY_CURRENT 3
#define CG_PIDS_MAX       4
#define CG_PIDS_CURRENT   5
#define CG_FREEZE         6
#define CG_MEM_REFUSED    7
#define CG_PID_REFUSED    8
#define CG_CPU_THROTTLES  9

struct sched_group;
struct proc;

struct cgroup {
  int used;
  int id;
  char name[CG_NAMELEN];
  struct cgroup *parent;
  struct sched_group *sg;   // cpu.weight and cpu.max live in the scheduler
  uint64 mem_max;           // pages; 0 is no limit
  uint64 mem_current;
  int pids_max, pids_current;
  int frozen;
  uint64 mem_refused, pid_refused;
};

void cginit(void);
struct cgroup *cg_root(void);
struct cgroup *cg_current(void);
struct cgroup *cg_find(const char *name);
struct cgroup *cg_at(int i);
int cg_create(const char *name, struct cgroup *parent, struct cgroup **out);
int cg_destroy(struct cgroup *cg);
int cg_set_limit(struct cgroup *cg, int which, uint64 value);
uint64 cg_get(struct cgroup *cg, int which);
int cg_join(struct proc *p, struct cgroup *cg);
void cg_leave(struct proc *p);
int cg_charge_page(struct proc *p);
void cg_uncharge_page(struct proc *p, int n);
void cg_walk_procs(struct cgroup *cg, void (*fn)(int pid, void *), void *arg);

#endif
