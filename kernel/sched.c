//
// weighted fair scheduling.
//
// every runnable task carries a virtual time: the processor time it
// has had, divided by its weight. The task with the least of it runs
// next. A task with twice the weight gains virtual time half as fast
// and so runs twice as long for the same rise, which is the whole of
// the fairness rule -- there is no priority level anywhere, only a
// weight and a clock.
//
// the runnable tasks of a hart sit in a red-black tree ordered by
// that virtual time (rbtree.c), so picking the next one is reading
// the leftmost node. Each hart has its own tree and its own lock,
// because a single queue with a single lock is the thing that stops
// a machine with thirty-two harts from being thirty-two harts; a
// task is put on the least loaded hart when it wakes, and an idle
// hart steals from the busiest.
//
// the awkward part of any scheme like this is a task that has been
// asleep: its virtual time stopped while everyone else's rose, so it
// would own the processor for as long as it had been away. It is
// given the queue's minimum instead, less half a latency period as
// credit for having waited -- enough to go first, not enough to run
// away with the machine.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "defs.h"
#include "rbtree.h"
#include "intc.h"
#include "platform.h"

// the task a tree node belongs to
#define rb_owner(n) ((struct proc *)((char *)(n) - __builtin_offsetof(struct proc, se.rb)))

// how long every runnable task should wait at most before it runs,
// and the least it is worth switching for. With more tasks than
// latency/granularity the period stretches rather than the slices
// shrinking to nothing: switching costs more than it buys below a
// few milliseconds.
//
// the scheduler's clock is the hardware timer, counted in its own
// units rather than nanoseconds. Converting would mean two 64-bit
// divisions every time the clock is read, and it is read several
// times per context switch; the units cancel out of everything here
// except these three constants, which are converted once.
#define SCHED_LATENCY_MS   24
#define MIN_GRANULARITY_MS  3
#define WAKEUP_GRANULARITY_MS 3
// how recently a task must have run for moving it to cost more than
// it gains. Linux calls this sched_migration_cost and sets it at half
// a millisecond.
#define MIGRATION_COST_US 500

static uint64 sched_latency;      // in timer ticks
static uint64 min_granularity;
static uint64 wakeup_granularity;
static uint64 migration_cost;

static inline uint64
sched_now(void)
{
  return r_time();
}

#define NICE0_WEIGHT 1024

// weight = 1024 / 1.25^nice, which makes one step of nice worth about
// ten per cent of the processor between two tasks. These are Linux's
// numbers, and they are a table rather than a calculation because the
// scheduler must not do arithmetic it cannot afford on every tick.
static const uint32 nice_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */  9548,  7620,  6100,  4904,  3906,
    /*  -5 */  3121,  2501,  1991,  1586,  1277,
    /*   0 */  1024,   820,   655,   526,   423,
    /*   5 */   335,   272,   215,   172,   137,
    /*  10 */   110,    87,    70,    56,    45,
    /*  15 */    36,    29,    23,    18,    15,
};

// 2^32 / weight, so that time / weight is a multiply and a shift.
// A division is not something to do four times per context switch on
// a machine that has to emulate it.
static uint32 nice_inv[40];

// ---- groups -------------------------------------------------------
//
// a group is a set of tasks that shares one place in a hart's tree.
// The hart is divided between the groups first and between the tasks
// of a group second, which is what makes a virtual machine's share of
// the processor a property of the machine rather than of how many
// vcpus somebody gave it. Two levels, no more: the root, and the
// groups. Anything that is in no group is in the root and competes
// with the groups directly, which is what a task of the host should
// do.
//
// each group has a queue of its own on each hart, and that queue's
// entity is what sits in the hart's tree. Its virtual time rises with
// the time its tasks use, divided by the group's weight; a task's
// virtual time rises with the time it uses, divided by its own weight
// and within its group. Neither level knows about the other beyond
// that.

struct sched_group;

struct gq {                 // one group's queue, on one hart
  struct sched_ent se;      // its place in the hart's tree
  struct rb_root tree;      // its tasks waiting on this hart
  uint64 min_vruntime;
  uint64 load;              // the weights of those tasks
  int nr_running;
  struct proc *curr;        // its task on this hart's processor, or 0
  struct sched_group *sg;
  int cpu;
  uint64 budget;            // processor time drawn from the group and
                            // not yet used, in the timer's units
  int throttled;            // out of budget: off the hart's tree
};

struct sched_group {
  struct spinlock lock;
  int used;
  int id;
  char name[SCHED_GROUP_NAMELEN];
  uint32 weight, inv_weight;   // cpu.weight
  // cpu.max: `quota' of processor time every `period'. quota 0 means
  // no limit, which is what a group has until somebody says otherwise.
  uint64 quota, period;
  uint64 pool;                 // what is left of the quota this period
  uint64 period_end;
  uint64 throttles;            // how often it has run out
  uint64 exec;                 // processor time its tasks have had
  int frozen;                  // not allowed to run at all
  int dying;                   // destroyed, and waiting for its last task
  int ntasks;
  struct gq q[NCPU];
};

static struct sched_group groups[NSCHEDGROUP];
static struct spinlock groups_lock;

struct rq {
  struct spinlock lock;
  struct rb_root tree;   // the entities waiting here: tasks and groups
  uint64 min_vruntime;   // never goes backwards
  uint64 load;           // the weights of those entities, summed
  int nr_running;
  struct proc *curr;     // the task on this hart now, or 0
  struct gq *curr_gq;    // ... and the group it belongs to, or 0
  uint64 switches;
  uint64 steals;
  uint64 y_resched, y_tick, preempts;
} __attribute__((aligned(64)));

static struct rq rqs[NCPU];

// how much of a group's quota a hart draws at a time. Drawing exactly
// what is used would mean taking the group's lock on every tick; this
// is a slice taken once and spent locally.
#define BANDWIDTH_SLICE_MS 5
// and the default period of cpu.max, which the plan sets at a tenth
// of a second: short enough that a limit is felt, long enough that
// the pool is not taken apart on every tick.
#define BANDWIDTH_PERIOD_MS 100
static uint64 bandwidth_slice;
static uint64 bandwidth_period;

uint32
sched_weight_of(int nice)
{
  if (nice < -20)
    nice = -20;
  if (nice > 19)
    nice = 19;
  return nice_weight[nice + 20];
}

void
schedinit(void)
{
  uint64 hz = plat.timebase ? plat.timebase : 10000000;

  sched_latency = hz * SCHED_LATENCY_MS / 1000;
  bandwidth_slice = hz * BANDWIDTH_SLICE_MS / 1000;
  bandwidth_period = hz * BANDWIDTH_PERIOD_MS / 1000;
  min_granularity = hz * MIN_GRANULARITY_MS / 1000;
  wakeup_granularity = hz * WAKEUP_GRANULARITY_MS / 1000;
  migration_cost = hz * MIGRATION_COST_US / 1000000;
  for (int i = 0; i < 40; i++)
    nice_inv[i] = (uint32)((1UL << 32) / nice_weight[i]);

  for (int i = 0; i < NCPU; i++) {
    initlock(&rqs[i].lock, "rq");
    memset(&rqs[i].tree, 0, sizeof(rqs[i].tree));
    rqs[i].min_vruntime = 0;
    rqs[i].load = 0;
    rqs[i].nr_running = 0;
    rqs[i].curr = 0;
    rqs[i].curr_gq = 0;
  }
  schedgroupinit();
}

// virtual time rises as real time divided by weight, in units where
// an entity of weight 1024 gains one unit of virtual time per unit of
// real time.
static uint64
virtual(uint64 delta, struct sched_ent *e)
{
  if (e->weight == NICE0_WEIGHT)
    return delta;
  // delta * 1024 / weight, by the reciprocal the entity carries
  uint64 x = delta * NICE0_WEIGHT;
  return (x >> 32) * e->inv_weight + (((x & 0xffffffffUL) * e->inv_weight) >> 32);
}

// the entity that stands for p in a hart's tree: p itself if it is in
// the root, otherwise its group's queue on that hart.
static struct gq *
group_q(struct rq *rq, struct proc *p)
{
  return p->sg ? &p->sg->q[rq - rqs] : 0;
}

static struct sched_ent *
root_ent(struct rq *rq, struct proc *p)
{
  struct gq *q = group_q(rq, p);

  return q ? &q->se : &p->se;
}

// a share of a period, which stretches rather than letting slices
// shrink below the point where switching costs more than it buys.
static uint64
share_of(int nr, uint64 load, uint32 weight)
{
  uint64 period = sched_latency;
  uint64 n = nr > 0 ? (uint64)nr : 1;

  if (n * min_granularity > period)
    period = n * min_granularity;
  if (load == 0)
    return period;
  return period * weight / load;
}

// the slice this task should get before the hart looks again: its
// group's share of the hart, and its share of its group.
static uint64
slice_of(struct rq *rq, struct proc *p)
{
  struct gq *q = group_q(rq, p);
  uint64 slice;

  if (q == 0)
    return share_of(rq->nr_running, rq->load, p->se.weight);
  slice = share_of(rq->nr_running, rq->load, q->se.weight);
  if (q->load == 0)
    return slice;
  slice = slice * p->se.weight / q->load;
  return slice < min_granularity ? min_granularity : slice;
}

// the minimum of a tree never moves backwards: it is what a waking
// entity is placed against, and a queue that let it fall would hand
// the processor to whoever slept longest.
static void
update_min_at(uint64 *min, struct rb_root *tree, struct sched_ent *curr)
{
  uint64 v = curr ? curr->vruntime : 0;
  struct rb_node *n = rb_first(tree);

  if (n) {
    uint64 leftmost = n->key;
    if (curr == 0 || leftmost < v)
      v = leftmost;
  }
  if (curr == 0 && n == 0)
    return;
  if (v > *min)
    *min = v;
}

static void
update_min(struct rq *rq)
{
  update_min_at(&rq->min_vruntime, &rq->tree,
                rq->curr ? root_ent(rq, rq->curr) : 0);
}

static void group_charge(struct rq *rq, struct gq *q, uint64 delta);

// charge the running task for the time it has had, at both levels.
// rq->lock held.
static void
update_curr(struct rq *rq, uint64 now)
{
  struct proc *p = rq->curr;

  if (p == 0)
    return;
  if (now <= p->exec_start) {
    p->exec_start = now;
    return;
  }
  uint64 delta = now - p->exec_start;
  p->exec_start = now;
  p->sum_exec += delta;
  p->se.vruntime += virtual(delta, &p->se);

  struct gq *q = rq->curr_gq;
  if (q) {
    update_min_at(&q->min_vruntime, &q->tree, &p->se);
    q->se.vruntime += virtual(delta, &q->se);
    group_charge(rq, q, delta);
  }
  update_min(rq);
}

// where an entity starts when it becomes runnable. `waking` is 0 for
// one that has just been made (it starts level with everyone else and
// is charged a slice so it cannot preempt its parent at once) and 1
// for one that has been away.
static void
place_ent(struct sched_ent *e, uint64 min, uint64 slice, int waking)
{
  uint64 v = min;

  if (waking) {
    // credit for having waited, bounded so that a long sleep is worth
    // no more than a short one.
    //
    // one granularity, not half a latency period. The credit and the
    // threshold below which a wakeup does not preempt are the same
    // quantity seen from two sides: make the credit the larger and
    // every task that has slept at all arrives ahead of the one
    // running and takes the processor from it. That is not
    // interactivity, it is a context switch per wakeup -- on this
    // machine, six hundred thousand of them in a run of usertests
    // that needs forty-six thousand.
    uint64 credit = min_granularity;
    v = v > credit ? v - credit : 0;
    if (e->vruntime > v)
      v = e->vruntime;   // never gain by sleeping
  } else {
    v += virtual(slice, e);
  }
  e->vruntime = v;
}

static void
place(struct rq *rq, struct proc *p, int waking)
{
  struct gq *q = group_q(rq, p);

  if (q == 0) {
    place_ent(&p->se, rq->min_vruntime, slice_of(rq, p), waking);
    return;
  }
  place_ent(&p->se, q->min_vruntime,
            share_of(q->nr_running, q->load, p->se.weight), waking);
}

// nr_running and load count everything this hart has to run, the one
// on the processor included. What is on the processor is not in the
// tree -- there is nowhere for it to be, it is running -- but leaving
// it out of the counts would tell pick_cpu that a hart running flat
// out was idle, and every task would be sent to the same one.
//
// so an entity is counted from the moment it becomes this hart's
// until the moment it stops being it, and being in the tree is a
// separate question from being counted. Keeping the two apart matters
// more than it sounds: something counted once and taken off twice
// leaves the hart believing it carries a load it does not, for as
// long as it lives, and every slice computed from that load is
// wrong. The weight it was counted for is remembered with it, because
// nice can change while it is running.
static void
ent_count(uint64 *load, int *nr, struct sched_ent *e)
{
  if (e->counted)
    return;
  e->load_weight = e->weight;
  *load += e->load_weight;
  (*nr)++;
  e->counted = 1;
}

static void
ent_uncount(uint64 *load, int *nr, struct sched_ent *e)
{
  if (!e->counted)
    return;
  *load -= e->load_weight;
  (*nr)--;
  e->counted = 0;
}

static void
ent_reweight(uint64 *load, struct sched_ent *e, uint32 w, uint32 inv)
{
  e->weight = w;
  e->inv_weight = inv;
  if (e->counted && e->load_weight != w) {
    *load -= e->load_weight;
    *load += w;
    e->load_weight = w;
  }
}

static void
ent_insert(struct rb_root *tree, struct sched_ent *e, int tag)
{
  if (e->on_rq)
    return;
  e->rb.tag = tag;
  rb_insert(tree, &e->rb, e->vruntime);
  e->on_rq = 1;
}

static void
ent_erase(struct rb_root *tree, struct sched_ent *e)
{
  if (!e->on_rq)
    return;
  rb_erase(tree, &e->rb);
  e->on_rq = 0;
}

static void
reweight(struct rq *rq, struct proc *p, uint32 w)
{
  int nice = p->nice < -20 ? -20 : p->nice > 19 ? 19 : p->nice;
  struct gq *q = group_q(rq, p);

  ent_reweight(q ? &q->load : &rq->load, &p->se, w, nice_inv[nice + 20]);
}

// a group is on a hart's tree when it has something to run there and is
// not out of its bandwidth. A frozen group stays on it: its tasks have to
// be able to *reach* the place where they stop (freeze_point below), and
// a task that cannot run cannot get there.
static void
group_activate(struct rq *rq, struct gq *q)
{
  if (q->nr_running == 0 || q->throttled)
    return;
  if (!q->se.counted) {
    // it has been away: it comes back at the hart's minimum, with the
    // weight the group has now.
    q->se.weight = q->sg->weight;
    q->se.inv_weight = q->sg->inv_weight;
    place_ent(&q->se, rq->min_vruntime, 0, 1);
    ent_count(&rq->load, &rq->nr_running, &q->se);
  }
  if (rq->curr_gq != q)
    ent_insert(&rq->tree, &q->se, SE_GROUP);
}

// and off it: frozen, throttled, or with nothing left to run here
static void
group_deactivate(struct rq *rq, struct gq *q)
{
  ent_erase(&rq->tree, &q->se);
  ent_uncount(&rq->load, &rq->nr_running, &q->se);
}

static void
enqueue(struct rq *rq, struct proc *p)
{
  struct gq *q = group_q(rq, p);

  if (q == 0) {
    ent_count(&rq->load, &rq->nr_running, &p->se);
    ent_insert(&rq->tree, &p->se, SE_TASK);
    return;
  }
  ent_count(&q->load, &q->nr_running, &p->se);
  ent_insert(&q->tree, &p->se, SE_TASK);
  group_activate(rq, q);
}

static void
dequeue(struct rq *rq, struct proc *p)
{
  struct gq *q = group_q(rq, p);

  ent_erase(q ? &q->tree : &rq->tree, &p->se);
}

// off the tree and onto the processor: still this hart's to run, and
// still counted as its
static void
take(struct rq *rq, struct proc *p)
{
  dequeue(rq, p);
}

// and the other half: the task is no longer this hart's at all
static void
uncount(struct rq *rq, struct proc *p)
{
  struct gq *q = group_q(rq, p);

  if (q == 0) {
    ent_uncount(&rq->load, &rq->nr_running, &p->se);
    return;
  }
  ent_uncount(&q->load, &q->nr_running, &p->se);
  if (q->nr_running == 0)
    group_deactivate(rq, q);
}

// ---- bandwidth ----------------------------------------------------
//
// cpu.max: a group may use `quota' of processor time every `period',
// counted across every hart at once. A hart draws a slice of that
// into its own queue and spends it there, so the group's lock is
// taken once every few milliseconds rather than on every tick; when
// the pool is empty the group comes off every hart's tree until the
// period turns over.
//
// this is the one control here that can make a task wait while a hart
// is idle, which is the whole point of it: a limit nobody enforces
// when the machine is quiet is not a limit.

static int
group_draw(struct gq *q)
{
  struct sched_group *g = q->sg;
  uint64 now = sched_now();
  int got = 0;

  acquire(&g->lock);
  if (g->quota == 0) {
    release(&g->lock);
    q->budget = bandwidth_slice;   // no limit: never runs out
    return 1;
  }
  if (now >= g->period_end) {
    g->period_end = now + g->period;
    g->pool = g->quota;
  }
  if (g->pool > 0) {
    uint64 take = g->pool < bandwidth_slice ? g->pool : bandwidth_slice;
    g->pool -= take;
    q->budget += take;
    got = 1;
  }
  release(&g->lock);
  return got;
}

// charge a group for time one of its tasks has used, and say whether
// it may go on. rq->lock held.
static void
group_charge(struct rq *rq, struct gq *q, uint64 delta)
{
  struct sched_group *g = q->sg;

  // what the group has used, which is what `cpu.stat' reports and
  // what a test asking whether two weights were honoured compares
  __atomic_add_fetch(&g->exec, delta, __ATOMIC_RELAXED);
  if (__atomic_load_n(&g->quota, __ATOMIC_RELAXED) == 0)
    return;   // no ceiling, and nothing to count against one

  // every unit of it has to come out of the budget, not just the
  // first: charging one draw for an interval longer than a draw is
  // how a group limited to a quarter of a processor gets three.
  while (delta > 0) {
    if (q->budget >= delta) {
      q->budget -= delta;
      return;
    }
    delta -= q->budget;
    q->budget = 0;
    if (!group_draw(q))
      break;
  }
  if (delta == 0)
    return;

  if (!q->throttled) {
    q->throttled = 1;
    acquire(&g->lock);
    g->throttles++;
    release(&g->lock);
    group_deactivate(rq, q);
    // the task on the processor has to come off as well; the hart it
    // is on is asked to look again.
    if (rq->curr_gq == q) {
      if (rq == &rqs[cpuid()])
        sched_need_resched();
      else
        sched_kick(rq - rqs);
    }
  }
}

// the period turned over: every hart's queue of this group may run
// again. Called from the tick, without rq locks held.
static void
group_unthrottle(struct sched_group *g)
{
  for (int i = 0; i < plat.ncpu; i++) {
    struct gq *q = &g->q[i];
    if (!q->throttled)
      continue;
    struct rq *rq = &rqs[i];
    acquire(&rq->lock);
    q->throttled = 0;
    q->budget = 0;
    if (q->nr_running > 0)
      group_activate(rq, q);
    int idle = rq->curr == 0 && q->nr_running > 0;
    release(&rq->lock);
    if (idle && i != cpuid())
      sched_kick(i);
  }
}

// once a tick: hand every limited group its quota back when its period
// is over, and let anything that was throttled run again.
void
sched_bandwidth_tick(void)
{
  uint64 now = sched_now();

  for (int i = 0; i < NSCHEDGROUP; i++) {
    struct sched_group *g = &groups[i];
    int wake = 0;
    if (!g->used || g->quota == 0)
      continue;
    acquire(&g->lock);
    if (now >= g->period_end) {
      g->period_end = now + g->period;
      g->pool = g->quota;
      wake = 1;
    }
    release(&g->lock);
    if (wake)
      group_unthrottle(g);
  }
}

// the harts a task may run on. An empty or impossible mask is read as
// "anywhere": affinity is a preference the caller states, never a way
// to make a task unrunnable.
static inline uint64
allowed(struct proc *p)
{
  uint64 all = plat.ncpu >= 64 ? ~0UL : (1UL << plat.ncpu) - 1;
  uint64 m = p->cpus & all;
  return m ? m : all;
}

static int
sched_may_run(struct proc *p, int cpu)
{
  return (allowed(p) >> cpu) & 1;
}

// the hart a waking task should go to.
//
// almost always the one it last ran on. Moving it costs more than it
// looks: its memory is in that hart's caches, and putting it on
// another hart's queue means taking that hart's lock from this one,
// which is the expensive kind of lock. Tasks wake each other
// constantly -- every pipe write, every disk completion -- so this
// runs far more often than a context switch does, and it is not the
// place to be clever.
//
// it moves only when home is genuinely worse off: twice the load of
// the least busy hart, and at least one task of difference. Ordinary
// imbalance is left to an idle hart to fix by stealing, which it can
// do without holding anything up.
//
// the one exception is a task somebody has just woken. It goes to the
// hart of whoever woke it, if that hart has nothing else waiting.
// Two tasks passing messages are the common case of a wakeup, and the
// one that sent the message is usually about to block on the answer:
// putting the woken task there means it runs on a hart that is about
// to be free, on data that is in that hart's cache, without an
// interrupt having to be sent anywhere. Sent to its old hart instead
// it costs an inter-processor interrupt and a wakeup from wfi each
// way, and a pair of tasks that talk ends up ping-ponging across the
// machine. Linux calls this wake_affine and has for twenty years.
static int
pick_cpu(struct proc *p, int how)
{
  uint64 mask = allowed(p);
  int here = cpuid();
  int home = p->cpu >= 0 && p->cpu < plat.ncpu ? p->cpu : here;

  if (!((mask >> home) & 1)) {
    // affinity says not there. Take the first hart it may use.
    for (int i = 0; i < plat.ncpu; i++)
      if ((mask >> i) & 1) {
        home = i;
        break;
      }
  }
  // ... except for a vcpu. A vcpu woken by its sibling's IPI is not the
  // partner of a task about to block: the sibling goes on running, and the
  // two of them on one hart is the machine's parallelism halved until the
  // next tick's balance (linux-disk-smp: two loops at 1.7-1.8x one, 11 of 80
  // quiet runs). A vcpu takes its home hart or the least loaded one.
  if (how == SCHED_WAKE && here != home && ((mask >> here) & 1) &&
      rqs[here].nr_running <= 1 && p->vcpu == 0)
    return here;

  uint64 hl = rqs[home].load;
  int best = home;
  uint64 bestload = hl;

  // one task there -- almost certainly the one doing the waking, which
  // is about to block -- is not a reason to go anywhere. Sending the
  // woken task to an idle hart instead sounds better and is not: two
  // tasks that wake each other then sit on two harts and every message
  // between them costs an inter-processor interrupt and a wakeup from
  // wfi. Measured on usertests, that is 130 seconds against 460.
  if (rqs[home].nr_running <= 1)
    return home;
  for (int i = 0; i < plat.ncpu; i++) {
    if (((mask >> i) & 1) && rqs[i].load < bestload) {
      bestload = rqs[i].load;
      best = i;
    }
  }
  if (hl > bestload * 2 && rqs[home].nr_running > rqs[best].nr_running + 1)
    return best;
  return home;
}

// make p runnable and put it somewhere. p->lock held.
//
// `how` says where its virtual time comes from: SCHED_NEW for a task
// that has just been made, SCHED_WAKE for one that has been asleep.
// A task that merely gives up the processor keeps the time it has and
// goes back on its own hart's tree in sched_end, not here.
void
sched_enqueue(struct proc *p, int how)
{
  int cpu = pick_cpu(p, how);
  struct rq *rq = &rqs[cpu];

  acquire(&rq->lock);
  update_curr(rq, sched_now());
  reweight(rq, p, sched_weight_of(p->nice));
  p->cpu = cpu;
  place(rq, p, how == SCHED_WAKE);
  enqueue(rq, p);
  // somebody with less virtual time than the task running here should
  // not have to wait for its slice to end
  struct proc *cur = rq->curr;
  int preempt = cur && p->se.vruntime + wakeup_granularity < cur->se.vruntime;
  int idle = cur == 0;
  release(&rq->lock);

  if (idle) {
    // a hart with nothing to run is in wfi, and wfi ends at an
    // interrupt. Its tree has just gained a task, but nothing has
    // told it so: left alone it sleeps until the next timer tick,
    // which is a tenth of a second away.
    //
    // with one queue for the whole machine this could not happen --
    // whichever hart was awake took the next task off it. A queue per
    // hart means the task is waiting on a specific hart, and the hart
    // it is waiting on has to be woken. This one line is most of what
    // a run of usertests costs when it is missing: every wakeup that
    // lands on an idle hart pays a tick for it.
    if (cpu != cpuid())
      sched_kick(cpu);
  } else if (preempt) {
    rqs[cpu].preempts++;
    if (cpu != cpuid())
      sched_kick(cpu);
    else
      sched_need_resched();
  }
}

// change the harts a task may run on. p->lock held.
//
// a task already waiting on a hart it may no longer use is moved at
// once; one that is running there is asked to give the processor up,
// and lands somewhere allowed when it is put back.
int
sched_set_affinity(struct proc *p, uint64 mask)
{
  uint64 all = plat.ncpu >= 64 ? ~0UL : (1UL << plat.ncpu) - 1;

  if ((mask & all) == 0)
    return -1;
  p->cpus = mask;
  if (p->cpu < 0 || ((mask >> p->cpu) & 1))
    return 0;

  if (p->se.on_rq) {
    struct rq *old = &rqs[p->cpu];
    acquire(&old->lock);
    dequeue(old, p);
    release(&old->lock);

    int cpu = pick_cpu(p, SCHED_MOVE);
    struct rq *rq = &rqs[cpu];
    acquire(&rq->lock);
    // its virtual time belongs to the queue it came from
    if (p->se.vruntime < rq->min_vruntime)
      p->se.vruntime = rq->min_vruntime;
    p->cpu = cpu;
    enqueue(rq, p);
    int idle = rq->curr == 0;
    release(&rq->lock);
    if (idle && cpu != cpuid())
      sched_kick(cpu);
  } else if (p->state == RUNNING) {
    // it is on a hart it may not use. Ask that hart to look again;
    // sched_end sees the mask and puts it somewhere allowed.
    if (p->cpu == cpuid())
      sched_need_resched();
    else
      sched_kick(p->cpu);
  }
  return 0;
}

uint64
sched_affinity(struct proc *p)
{
  return allowed(p);
}

// the nice value has changed. Give it its weight now rather than at
// the next time it is put on a queue: a task that never blocks would
// otherwise keep the weight it was born with for as long as it ran.
void
sched_renice(struct proc *p)
{
  if (p->cpu < 0 || p->cpu >= plat.ncpu)
    return;
  struct rq *rq = &rqs[p->cpu];
  acquire(&rq->lock);
  update_curr(rq, sched_now());
  reweight(rq, p, sched_weight_of(p->nice));
  release(&rq->lock);
}

// what a node of a hart's tree will actually run: itself if it is a
// task, and the leftmost waiting task of the group if it is a group.
// The group's own entity stays where it is; it is `running' at the
// root level for as long as one of its tasks is on the processor.
static struct proc *
ent_task(struct rb_node *n)
{
  if (n->tag == SE_TASK)
    return rb_owner(n);
  struct gq *q = (struct gq *)n;
  struct rb_node *m = rb_first(&q->tree);
  return m ? rb_owner(m) : 0;
}

// a task that was on a processor a moment ago is still in that hart's
// caches, and moving it costs more than the move gains. This is what
// keeps two tasks that pass messages to each other from being pulled
// apart: the one that is waiting has only just run, because they take
// turns. A task that has been waiting behind another for a whole
// slice is a different matter, and that is what stealing is for.
static int
task_hot(struct proc *p, uint64 now)
{
  return now - p->exec_start < migration_cost;
}

// the task on `src' that would run last there and may run on `to':
// taken off src's trees and out of its counts, or 0. src->lock held.
//
// never the one running there, and inside a group rather than the
// group itself -- a group's queue belongs to its hart; its tasks do
// not.
static struct proc *
take_farthest(struct rq *src, int to)
{
  struct proc *p = 0;
  uint64 now = sched_now();

  for (struct rb_node *n = rb_last(&src->tree); n && p == 0; n = rb_prev(n)) {
    if (n->tag == SE_TASK) {
      struct proc *c = rb_owner(n);
      if (c == src->curr || !sched_may_run(c, to) || task_hot(c, now))
        continue;
      p = c;
    } else {
      struct gq *q = (struct gq *)n;
      for (struct rb_node *m = rb_last(&q->tree); m; m = rb_prev(m)) {
        struct proc *c = rb_owner(m);
        if (c == src->curr || !sched_may_run(c, to) || task_hot(c, now))
          continue;
        p = c;
        break;
      }
    }
  }
  if (p) {
    dequeue(src, p);
    uncount(src, p);
  }
  return p;
}

// put a task that has just been taken from somewhere else on this
// hart. dst->lock is taken and released here.
static void
put_here(struct rq *dst, struct proc *p, int cpu)
{
  acquire(&dst->lock);
  // its virtual time means nothing here, so line it up with this
  // queue rather than letting it hog or starve
  if (p->se.vruntime < dst->min_vruntime)
    p->se.vruntime = dst->min_vruntime;
  p->cpu = cpu;
  enqueue(dst, p);
  release(&dst->lock);
}

// is this hart idle? Asked by the timer, which has no reason to wake
// a hart that has nothing to do.
int
sched_hart_idle(void)
{
  struct rq *rq = &rqs[cpuid()];

  return rq->curr == 0 && rq->nr_running == 0;
}

// May this hart stop asking for timer interrupts altogether?
//
// Three things have to be true. One thing is running and nothing is waiting
// behind it, so there is nobody to preempt for and the slice that would end
// is dividing a processor between one task. That thing is a guest's vcpu, so
// the interrupt would not merely be a trap but an exit -- about sixteen
// microseconds here, and at a hundred a second most of what a guest busy
// computing pays for being a guest. And its group has no ceiling: a quota is
// enforced by charging the time as it is used (group_charge, from
// update_curr), and a hart that never takes an interrupt never gets there,
// so a capped group must keep its slice or it would overrun the cap.
//
// What is *not* needed is the tick for accounting: update_curr charges from
// the time counter whenever the task stops, not by counting ticks.
int
sched_tickless_ok(void)
{
  struct rq *rq = &rqs[cpuid()];
  struct proc *p = rq->curr;

  // nr_running counts the task on the processor too (see the note above
  // ent_count), so one means this task and nobody else.
  if (p == 0 || rq->nr_running > 1 || p->vcpu == 0)
    return 0;
  struct gq *q = rq->curr_gq;
  if (q && q->sg && __atomic_load_n(&q->sg->quota, __ATOMIC_RELAXED) != 0)
    return 0;
  return 1;
}


// once a tick, on every hart: a hart with tasks waiting and an idle
// hart next to it moves one over. Stealing covers the case where the
// idle hart is awake and looking; this covers the case where it is
// not, because a hart in wfi does not look at anything until it is
// interrupted.
void
sched_balance(void)
{
  int me = cpuid();
  struct rq *rq = &rqs[me];
  int to = -1;

  // something waiting here and a hart with nothing at all over there.
  // Two is enough: one of them is on the processor and the other is
  // waiting for it, and the hart next door is asleep. What stops this
  // from pulling apart two tasks that pass messages -- which would
  // cost far more than it gained -- is that such a pair is never both
  // runnable at once, and that take_farthest will not move a task
  // that was running a moment ago.
  if (rq->nr_running < 2)
    return;
  for (int i = 0; i < plat.ncpu; i++) {
    if (i != me && rqs[i].nr_running == 0) {
      to = i;
      break;
    }
  }
  if (to < 0)
    return;

  acquire(&rq->lock);
  struct proc *p = rq->nr_running >= 2 ? take_farthest(rq, to) : 0;
  if (p)
    rq->steals++;
  release(&rq->lock);
  if (p == 0)
    return;

  put_here(&rqs[to], p, to);
  sched_kick(to);
}

// an idle hart takes work from the busiest one. Only a task that is
// merely waiting is taken -- never the one running there -- and never
// the leftmost, so the busy hart keeps something to run.
static struct proc *
steal(int me)
{
  int from = -1;
  uint64 most = 0;

  for (int i = 0; i < plat.ncpu; i++) {
    if (i == me)
      continue;
    // something waiting behind the one it is running
    if (rqs[i].nr_running > 1 && rqs[i].load > most) {
      most = rqs[i].load;
      from = i;
    }
  }
  if (from < 0)
    return 0;

  struct rq *src = &rqs[from];
  struct proc *p = 0;
  acquire(&src->lock);
  if (src->nr_running > 1) {
    p = take_farthest(src, me);
    if (p)
      src->steals++;
  }
  release(&src->lock);
  if (p == 0)
    return 0;

  put_here(&rqs[me], p, me);
  return p;
}

// the leftmost thing on this hart, and the task it stands for, taken
// off both trees. rq->lock held.
static struct proc *
pick_locked(struct rq *rq)
{
  struct rb_node *n;
  struct proc *p;

  // a group on the tree with nothing in it should not be there; take
  // it off and look again rather than telling the hart it is idle.
  for (;;) {
    n = rb_first(&rq->tree);
    if (n == 0)
      return 0;
    p = ent_task(n);
    if (p)
      break;
    group_deactivate(rq, (struct gq *)n);
  }
  if (n->tag == SE_GROUP) {
    struct gq *q = (struct gq *)n;
    ent_erase(&rq->tree, &q->se);   // it is running at the root level
    rq->curr_gq = q;
    q->curr = p;
  } else {
    rq->curr_gq = 0;
  }
  take(rq, p);
  return p;
}

// the next task for this hart, taken off the queue, or 0. The caller
// has not got p->lock; it takes it and checks the state, as the old
// scheduler did.
// F8: everything queued on hart `me' goes to hart 0, which is never parked.
// Only what is here: stealing would pull work from the harts that stay.
void
sched_offline_drain(int me)
{
  struct rq *rq = &rqs[me];
  struct proc *p;
  int n = 0;
  for (;;) {
    acquire(&rq->lock);
    update_curr(rq, sched_now());
    p = pick_locked(rq);
    release(&rq->lock);
    if (p == 0)
      break;
    put_here(&rqs[0], p, 0);
    n++;
  }
  if (n)
    printk("machine: hart %d handed %d task(s) to hart 0\n", me, n);
}

struct proc *
sched_pick(void)
{
  int me = cpuid();
  struct rq *rq = &rqs[me];
  struct proc *p = 0;

  acquire(&rq->lock);
  update_curr(rq, sched_now());
  p = pick_locked(rq);
  release(&rq->lock);

  if (p == 0 && steal(me)) {
    acquire(&rq->lock);
    p = pick_locked(rq);
    release(&rq->lock);
  }
  return p;
}

// a task was taken off the tree to be run and then turned out not to
// be runnable after all -- another hart stopped it, or reaped it,
// between the pick and the lock. It is on no tree and still counted
// as this hart's, which is a task that never runs again and a load
// that never goes away. Put it back, or let it go.
void
sched_unpick(struct proc *p)
{
  struct rq *rq = &rqs[cpuid()];

  acquire(&rq->lock);
  struct gq *q = rq->curr_gq;
  if (q && q->curr == p) {
    q->curr = 0;
    rq->curr_gq = 0;
  }
  if (p->state == RUNNABLE)
    enqueue(rq, p);
  else
    uncount(rq, p);
  if (q)
    group_activate(rq, q);
  release(&rq->lock);
}

// when the task running on this hart has had its slice, or 0 if
// nothing is running. The timer is programmed for this as well as for
// the periodic tick (trap.c): a tick of ten milliseconds cannot
// enforce a slice of three, and a scheduler whose slices are only
// looked at every tick shares the processor to the nearest tick.
uint64
sched_slice_deadline(void)
{
  struct rq *rq = &rqs[cpuid()];

  return rq->curr ? rq->curr->slice_end : 0;
}

// p is about to run on this hart
void
sched_begin(struct proc *p)
{
  struct rq *rq = &rqs[cpuid()];

  acquire(&rq->lock);
  p->cpu = cpuid();
  p->exec_start = sched_now();
  p->slice_end = p->exec_start + slice_of(rq, p);
  rq->curr = p;
  rq->switches++;
  uint64 deadline = p->slice_end;
  release(&rq->lock);
  // the timer may be set for a tick that is further off than the end
  // of this slice
  timer_slice(deadline);
}

// p has stopped running on this hart. If it is still runnable it goes
// back in the tree here rather than before the switch: a task that is
// in the tree and on the processor at the same time is counted twice,
// and the hart's idea of its own load is what slices are computed
// from.
void
sched_end(struct proc *p)
{
  int me = cpuid();
  struct rq *rq = &rqs[me];
  int move = 0;

  acquire(&rq->lock);
  update_curr(rq, sched_now());
  if (rq->curr == p)
    rq->curr = 0;
  struct gq *q = rq->curr_gq;
  if (q && q->curr == p) {
    q->curr = 0;
    rq->curr_gq = 0;
  }
  if (p->state == RUNNABLE && sched_may_run(p, me)) {
    enqueue(rq, p);
  } else {
    uncount(rq, p);
    move = p->state == RUNNABLE;   // affinity says: not here
  }
  // the group goes back on the hart's tree if it still has anything
  // here and is allowed to run
  if (q)
    group_activate(rq, q);
  release(&rq->lock);

  if (!move)
    return;

  int cpu = pick_cpu(p, SCHED_MOVE);
  struct rq *dst = &rqs[cpu];
  acquire(&dst->lock);
  if (p->se.vruntime < dst->min_vruntime)
    p->se.vruntime = dst->min_vruntime;
  p->cpu = cpu;
  enqueue(dst, p);
  int idle = dst->curr == 0;
  release(&dst->lock);
  if (idle && cpu != me)
    sched_kick(cpu);
}

// the timer tick: has the running task had its slice?
int
sched_tick_expired(void)
{
  struct rq *rq = &rqs[cpuid()];
  uint64 now = sched_now();
  int over = 0;

  acquire(&rq->lock);
  update_curr(rq, now);
  struct proc *p = rq->curr;
  if (p) {
    struct sched_ent *e = root_ent(rq, p);
    if (now >= p->slice_end && rq->nr_running > 0)
      over = 1;
    else if (rq->nr_running > 0) {
      // or somebody waiting has fallen far enough behind. At the root
      // level that is the entity standing for this task, and inside
      // its group it is the task itself.
      struct rb_node *n = rb_first(&rq->tree);
      if (n && e->vruntime > n->key + min_granularity)
        over = 1;
      struct gq *q = rq->curr_gq;
      if (!over && q) {
        n = rb_first(&q->tree);
        if (n && p->se.vruntime > n->key + min_granularity)
          over = 1;
      }
    }
  }
  release(&rq->lock);
  return over;
}

// why a task gave up the processor, for /proc/schedstat
void
sched_count_yield(int resched)
{
  struct rq *rq = &rqs[cpuid()];

  if (resched)
    rq->y_resched++;
  else
    rq->y_tick++;
}

// tell a hart that something with less virtual time is waiting for it
void
sched_kick(int cpu)
{
  if (cpu != cpuid())
    intc_ipi(cpu, IPI_RESCHED);
}

// and this hart, which will look when it next returns from a trap
void
sched_need_resched(void)
{
  mycpu()->resched = 1;
}

// ---- the group interface ------------------------------------------
//
// what a VM, a container or a cgroup holds on to. Groups are made and
// destroyed rarely and looked up by pointer, so there is one lock for
// the table and one per group for what changes while it runs.

void
schedgroupinit(void)
{
  initlock(&groups_lock, "schedgroups");
  for (int i = 0; i < NSCHEDGROUP; i++) {
    initlock(&groups[i].lock, "schedgroup");
    groups[i].id = i;
  }
}

// make a group. `weight' is on the nice-0 scale, 1024 being an equal
// share with a task of nice 0 in the root; 0 asks for that default.
struct sched_group *
sched_group_create(const char *name, uint32 weight)
{
  struct sched_group *g = 0;

  acquire(&groups_lock);
  for (int i = 0; i < NSCHEDGROUP; i++) {
    if (!groups[i].used) {
      g = &groups[i];
      break;
    }
  }
  if (g == 0) {
    release(&groups_lock);
    return 0;
  }
  g->used = 1;
  release(&groups_lock);

  acquire(&g->lock);
  int n = 0;
  if (name)
    for (; name[n] && n < SCHED_GROUP_NAMELEN - 1; n++)
      g->name[n] = name[n];
  g->name[n] = 0;
  g->weight = weight ? weight : NICE0_WEIGHT;
  g->inv_weight = (uint32)((1UL << 32) / g->weight);
  g->quota = 0;
  g->period = bandwidth_period;
  g->pool = 0;
  g->period_end = 0;
  g->throttles = 0;
  g->frozen = 0;
  g->dying = 0;
  g->exec = 0;
  g->ntasks = 0;
  for (int i = 0; i < NCPU; i++) {
    struct gq *q = &g->q[i];
    memset(q, 0, sizeof(*q));
    q->sg = g;
    q->cpu = i;
  }
  release(&g->lock);
  return g;
}

// a group by the number /proc/schedstat prints, or 0
struct sched_group *
sched_group_by_id(int id)
{
  if (id < 0 || id >= NSCHEDGROUP)
    return 0;
  return groups[id].used ? &groups[id] : 0;
}

int
sched_group_id(struct sched_group *g)
{
  return g ? g->id : -1;
}

// nobody is holding this group any more. Its slot goes back when its
// last task leaves and not before: a slot handed out again while a
// task is still counted in it would take that count with it, and the
// hart it was counted on would carry a load that nothing can remove.
static void
group_release(struct sched_group *g)
{
  int free_it;

  acquire(&g->lock);
  free_it = g->dying && g->ntasks == 0;
  release(&g->lock);
  if (!free_it)
    return;
  acquire(&groups_lock);
  g->used = 0;
  release(&groups_lock);
}

void
sched_group_destroy(struct sched_group *g)
{
  if (g == 0)
    return;
  acquire(&g->lock);
  g->dying = 1;
  release(&g->lock);
  group_release(g);
}

// put p in g (0 for the root). p->lock held.
//
// a task may be running when this is called -- `vmm' becomes a vcpu
// of a machine by calling vm_run, and it is its own process doing it
// -- so the move has to take it out of the counts of the place it is
// leaving and put it into the counts of the place it is joining, in
// that order and under the hart's lock. Changing p->sg on a task that
// is counted somewhere leaves that count behind for ever, because the
// code that removes it looks at p->sg to know where to remove it
// from.
void
sched_group_join(struct proc *p, struct sched_group *g)
{
  struct sched_group *from = p->sg;

  if (from == g)
    return;

  struct rq *rq = p->cpu >= 0 && p->cpu < plat.ncpu ? &rqs[p->cpu] : 0;
  if (rq) {
    acquire(&rq->lock);
    update_curr(rq, sched_now());
    int running = rq->curr == p;
    int counted = p->se.counted;
    struct gq *old = rq->curr_gq;

    if (counted) {
      dequeue(rq, p);
      uncount(rq, p);
    }
    if (running && old && old->curr == p) {
      old->curr = 0;
      rq->curr_gq = 0;
      group_activate(rq, old);   // it may still have tasks waiting
    }

    p->sg = g;

    if (counted) {
      // its virtual time belonged to the queue it has left
      place(rq, p, 1);
      enqueue(rq, p);
      if (running) {
        struct gq *q = group_q(rq, p);
        take(rq, p);
        if (q) {
          ent_erase(&rq->tree, &q->se);
          rq->curr_gq = q;
          q->curr = p;
        }
      }
    }
    release(&rq->lock);
  } else {
    p->sg = g;
  }

  if (from) {
    acquire(&from->lock);
    from->ntasks--;
    release(&from->lock);
    group_release(from);   // the one it left may have been waiting for it
  }
  if (g) {
    acquire(&g->lock);
    g->ntasks++;
    release(&g->lock);
  }
}

// cpu.weight
int
sched_group_set_weight(struct sched_group *g, uint32 weight)
{
  if (g == 0 || weight < 1 || weight > 1000000)
    return -1;
  acquire(&g->lock);
  g->weight = weight;
  g->inv_weight = (uint32)((1UL << 32) / weight);
  release(&g->lock);
  // every hart's copy takes the new weight when the group next comes
  // back on its tree; one that is on a tree now is reweighted here.
  for (int i = 0; i < plat.ncpu; i++) {
    struct rq *rq = &rqs[i];
    struct gq *q = &g->q[i];
    acquire(&rq->lock);
    if (q->se.counted)
      ent_reweight(&rq->load, &q->se, g->weight, g->inv_weight);
    release(&rq->lock);
  }
  audit("sched_group", g->name, (uint64)g->id, weight, 1);
  return 0;
}

uint32
sched_group_weight(struct sched_group *g)
{
  return g ? g->weight : NICE0_WEIGHT;
}

// cpu.max: `quota' microseconds of processor time every `period'
// microseconds, across the whole machine. quota 0 means no limit.
int
sched_group_set_max(struct sched_group *g, uint64 quota_us, uint64 period_us)
{
  uint64 hz = plat.timebase ? plat.timebase : 10000000;

  if (g == 0)
    return -1;
  if (period_us == 0)
    period_us = BANDWIDTH_PERIOD_MS * 1000;
  if (period_us < 1000 || period_us > 10000000)
    return -1;
  acquire(&g->lock);
  g->period = hz * period_us / 1000000;
  g->quota = quota_us ? hz * quota_us / 1000000 : 0;
  g->pool = g->quota;
  g->period_end = sched_now() + g->period;
  release(&g->lock);
  if (quota_us == 0)
    group_unthrottle(g);   // no limit: nothing stays throttled
  audit("cpu_max", g->name, quota_us, period_us, 1);
  return 0;
}

void
sched_group_max(struct sched_group *g, uint64 *quota_us, uint64 *period_us)
{
  uint64 hz = plat.timebase ? plat.timebase : 10000000;

  if (g == 0) {
    *quota_us = 0;
    *period_us = 0;
    return;
  }
  acquire(&g->lock);
  *quota_us = g->quota * 1000000 / hz;
  *period_us = g->period * 1000000 / hz;
  release(&g->lock);
}

// the cgroup `freeze' file, and `vmm pause': the group's tasks stay
// runnable and stay where they are, and nothing picks them.
int
sched_group_freeze(struct sched_group *g, int on)
{
  if (g == 0)
    return -1;
  acquire(&g->lock);
  g->frozen = on ? 1 : 0;
  release(&g->lock);

  for (int i = 0; i < plat.ncpu; i++) {
    struct rq *rq = &rqs[i];
    struct gq *q = &g->q[i];
    int kick = 0;
    acquire(&rq->lock);
    if (on) {
      // it keeps its place on every hart's tree: its tasks run on, as
      // far as the next boundary, and stop there.
      if (rq->curr_gq == q && i == cpuid())
        sched_need_resched();
    } else {
      group_activate(rq, q);
      kick = rq->curr == 0 && q->nr_running > 0;
    }
    release(&rq->lock);
    if (on && i != cpuid() && q->curr)
      sched_kick(i);
    if (kick && i != cpuid())
      sched_kick(i);
  }
  if (!on)
    wakeup(g);      // and the ones that had stopped at a boundary go on
  return 0;
}

int
sched_group_frozen(struct sched_group *g)
{
  return g ? g->frozen : 0;
}

// what a group has done, for /proc/schedstat and the stats files:
// tasks, weight, quota and period in microseconds, how often it has
// been throttled, whether it is frozen, and how many of its tasks are
// runnable now.
void
sched_group_stat(struct sched_group *g, uint64 *out, int n)
{
  uint64 hz = plat.timebase ? plat.timebase : 10000000;
  uint64 v[8];

  memset(v, 0, sizeof(v));
  if (g) {
    acquire(&g->lock);
    v[0] = g->ntasks;
    v[1] = g->weight;
    v[2] = g->quota * 1000000 / hz;
    v[3] = g->period * 1000000 / hz;
    v[4] = g->throttles;
    v[5] = g->frozen;
    release(&g->lock);
    for (int i = 0; i < plat.ncpu; i++)
      v[6] += g->q[i].nr_running;
    v[7] = __atomic_load_n(&g->exec, __ATOMIC_RELAXED) * 1000 / (hz / 1000);
  }
  for (int i = 0; i < n && i < 8; i++)
    out[i] = v[i];
}

const char *
sched_group_name(struct sched_group *g)
{
  return g ? g->name : "";
}

// every group, for /proc/schedstat: id, tasks, weight, quota and
// period in microseconds, throttles, frozen, runnable now, and the
// processor time it has used in microseconds. Nine numbers each;
// returns how many groups were written.
int
sched_group_list(uint64 *out, int max, char names[][SCHED_GROUP_NAMELEN])
{
  int n = 0;

  for (int i = 0; i < NSCHEDGROUP && n < max; i++) {
    struct sched_group *g = &groups[i];
    if (!g->used)
      continue;
    uint64 *o = &out[n * 9];
    o[0] = g->id;
    sched_group_stat(g, o + 1, 8);
    if (names)
      for (int k = 0; k < SCHED_GROUP_NAMELEN; k++)
        names[n][k] = g->name[k];
    n++;
  }
  return n;
}

void
sched_stats(uint64 *out, int n)
{
  for (int i = 0; i < NCPU && i * SCHED_STAT_FIELDS + 6 < n; i++) {
    acquire(&rqs[i].lock);
    uint64 *o = &out[i * SCHED_STAT_FIELDS];
    o[0] = rqs[i].nr_running;
    o[1] = rqs[i].load;
    o[2] = rqs[i].switches;
    o[3] = rqs[i].steals;
    o[4] = rqs[i].y_tick;
    o[5] = rqs[i].y_resched;
    o[6] = rqs[i].preempts;
    release(&rqs[i].lock);
  }
}
