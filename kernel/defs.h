#include "signal.h"
// clang-format off
struct mm;
struct sock;
struct pipe;
struct file;
struct tcpcb;
struct context;
struct file;
struct inode;
struct pipe;
struct proc;
struct spinlock;
struct sleeplock;
struct stat;
struct gendisk;

// console.c
void            consoleinit(void);
void            devnullinit(void);
void            virtio_disk_register(void);
void            blkinit(void);
uint32          crc32c(uint32 crc, const void *data, int len);
void            procfs_init(void);
void            ext4_init(void);
int             ext4_probe(struct gendisk *);
void            vfs_init(void);
void            root_mount(void);
int             idirread(struct inode *, uint64 *, char *, int, uint *, uchar *);
struct inode*   iget(uint, uint);
void            consoleintr(int);
void            consoleraw(int);
void            consputc(int);
void            consputs(const char *, int);

// exec.c
int             kexec(char*, char**);
int             kexecve(char *, char **, char **);

// file.c
struct file*    filealloc(void);
void            fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             filewrite(struct file*, uint64, int n);

// fs.c
struct inode*   dirlookup(struct inode*, char*, uint*);
struct inode*   idup(struct inode*);
void            iinit();
void            ilock(struct inode*);
void            iput(struct inode*);
void            iunlock(struct inode*);
void            iunlockput(struct inode*);
void            iupdate(struct inode*);
int             namecmp(const char*, const char*);
struct inode*   namei(char*);
struct inode*   nameiparent(char*, char*);
int             readi(struct inode*, int, uint64, uint, uint);
void            stati(struct inode*, struct stat*);
int             writei(struct inode*, int, uint64, uint, uint);
void            itrunc(struct inode*);

// kalloc.c
void*           kalloc(void);
void            kfree(void *);
void            kinit(void);

// pipe.c
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);

// printk.c
int             printk(char*, ...) __attribute__ ((format (printf, 1, 2)));
void            panic(char*) __attribute__((noreturn));
void            printkinit(void);

// proc.c
int             cpuid(void);
void            kexit(int);
int             kfork(void);
int             growproc(int, int);
void            proc_mapstacks(pagetable_t);
int             kkill(int);
int             killed(struct proc*);
void            setkilled(struct proc*);
struct cpu*     mycpu(void);
struct proc*    myproc();
void            procinit(void);
void            scheduler(void) __attribute__((noreturn));
void            sched(void);
void            sleep_prepare(void*);
void            sleep_intr(void);
void            sleep(void);
void            userinit(void);
int             kwait(uint64);
void            wakeup(void*);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_memset(int user_dst, uint64 dst, int c, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);

// swtch.S
void            swtch(struct context*, struct context*);

// spinlock.c
void            acquire(struct spinlock*);
int             holding(struct spinlock*);
void            initlock(struct spinlock*, char*);
void            release(struct spinlock*);
void            push_off(void);
void            pop_off(void);

// sleeplock.c
void            acquiresleep(struct sleeplock*);
int             tryacquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);
void            initsleeplock(struct sleeplock*, char*);

// string.c
int             memcmp(const void*, const void*, uint);
void*           memmove(void*, const void*, uint);
void*           memset(void*, int, uint);
char*           safestrcpy(char*, const char*, int);
int             strlen(const char*);
int             strncmp(const char*, const char*, uint);
char*           strncpy(char*, const char*, int);

// syscall.c
void            argint(int, int*);
int             argstr(int, char*, int);
void            argaddr(int, uint64 *);
int             fetchstr(uint64, char*, int);
int             fetchaddr(uint64, uint64*);
void            syscall();

// trap.c
extern uint     ticks;
void            trapinit(void);
void            trapinithart(void);
extern struct spinlock tickslock;
void            prepare_return(void);

// uart.c
void            uartinit(void);
void            uartintr(void);
void            uartwrite(char [], int);
void            uartputc_sync(int);
void            uartputs_sync(const char *, int);

// vm.c
void            kvminit(void);
void            kvminithart(void);
void            kvmmap(pagetable_t, uint64, uint64, uint64, int);
int             mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t     uvmcreate(void);
uint64          uvmalloc(pagetable_t, uint64, uint64, int);
uint64          uvmdealloc(pagetable_t, uint64, uint64);
void            uvmfree(pagetable_t, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
pte_t *         walk(pagetable_t, uint64, int);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(struct mm *, uint64, char *, uint64);
int             copyin(struct mm *, char *, uint64, uint64);
int             copyinstr(struct mm *, char *, uint64, uint64);

// virtio_disk.c
void            virtio_disk_init(void);
void            virtio_disk_intr(int);
int             virtio_disk_present(int);
int             virtio_disk_count(void);
uint64          virtio_disk_capacity(int);

// trap.c
int             devintr(void);

// slab.c (slab.h has the cache interface)
void            slabinit(void);

// kstack.c
void            kstackinit(void);
uint64          kstack_alloc(void);
void            kstack_free(uint64);
void            kstack_flush_ack(void);
void            kstack_drain(void);
void            kstack_stats(uint64 *, uint64 *);

// proc.c (more)
struct file;
struct fdtable;
struct fsinfo;
struct fdtable *fdtable_alloc(void);
struct fdtable *fdtable_dup(struct fdtable *);
struct fdtable *fdtable_copy(struct fdtable *);
void            fdtable_put(struct fdtable *);
struct fsinfo * fsinfo_alloc(void);
struct fsinfo * fsinfo_dup(struct fsinfo *);
struct fsinfo * fsinfo_copy(struct fsinfo *);
void            fsinfo_put(struct fsinfo *);
struct inode *  proc_cwd(struct proc *);
struct inode *  proc_set_cwd(struct proc *, struct inode *);
struct file *   fdtake(struct proc *, int);
struct file *   fdput(struct proc *, int, struct file *, int);
int             fdalloc(struct file *);
int             fdalloc_from(struct file *, int);
int             fdtable_grow_to(struct proc *, int);
int             fileready(struct file *, int);
int             piperready(struct pipe *, int);
int             sockready(struct sock *, int);
int             consoleready(int);
int             consoleioctl(uint64, uint64);
int             fdflags_get(struct proc *, int);
int             fdflags_set(struct proc *, int, int);
void            fdtable_cloexec(struct proc *);
struct file *   fdget(struct proc *, int);
uint64          proc_count(void);
void            procinit_early(void);

// vm.c (runtime kernel mappings)
int             kvmmap_rt(uint64 va, uint64 pa, uint64 sz, int perm);
uint64          kvmunmap_rt(uint64 va, uint64 sz);
uint64          kvmprune_rt(uint64 va);

// signal.c
struct sigq;
struct sighand;
void            siginit(void);
struct sighand *sighand_alloc(void);
struct sighand *sighand_copy(struct sighand *);
struct sighand *sighand_share(struct sighand *);
int             kclone(uint64, uint64, uint64, uint64, uint64);
int             proc_nthreads(struct proc *);
void            tgroup_kill_others(struct proc *);
void            tgroup_take_over(struct proc *);
void            wake_proc(struct proc *);
void            sleep_cancel(void);
int             wakeup_n(void *, int);
// futex.c
int             futex_wait(uint64, uint32, uint64);
int             futex_wake(uint64, int);
int             futex_requeue(uint64, int, uint64, int);
void            futex_timeout(struct proc *);
void            sighand_put(struct sighand *);
void            sighand_exec(struct proc *);
void            sigq_free(struct sigq *);
int             signal_send(struct proc *, int, siginfo_t *);
int             signal_kill(int, int, siginfo_t *);
int             signal_tkill(int, int, int);
void            signal_fault(struct proc *, int, int, uint64);
int             signal_pending(struct proc *);
void            signal_timers(void);
void            do_signals(void);

// proc.c (signals and job control)
void            proc_make_runnable(struct proc *);
void            proc_notify_parent(struct proc *);
void            sched_stopped(void);
void            sigq_free_all(struct proc *);
struct proc *   proc_get(int);
void            proc_release(struct proc *);
void            proc_iter_start(void);
struct proc *   proc_iter_next(struct proc *);
void            proc_iter_end(void);
int             kwait4(int, uint64, int, uint64);
int             proc_ppid(struct proc *);

// mm.c (mm.h has the address space interface)
void            mminit(void);
struct mm;
struct mm *     proc_mm_create(struct proc *);
void            proc_mm_free(struct mm *);
int             kthread_create(void (*)(void *), void *, const char *);
void            kthread_selftest(void);
void            uvmunmap_count(pagetable_t, uint64, uint64, uint64 *);
void            freewalk(pagetable_t);

// kalloc.c (the rest is in page.h)
void *          kalloc_order(int);
void            kfree_order(void *, int);
void *          kalloc_guest(int);
uint64          kmem_free_pages(void);

// rtc.c
void            rtcinit(void);
int             rtc_present(void);
uint64          clock_realtime_ns(void);
uint64          clock_monotonic_ns(void);
void            randominit(void);
void            random_bytes(void *, int);
void            random_add(const void *, int);

// net.c / virtio_net.c
void            netinit(void);
void            net_timer(void);
void            virtio_net_intr(int);
void            bridgeinit(void);

// fpu.S
void            fp_save(uint64 *);
void            fp_restore(uint64 *);
void            vec_save(char *, uint64 *);
void            vec_restore(char *, uint64 *);

// hyp.c
void            hypinit(void);
void            hypinithart(void);
void            hyp_tick(void);
uint64          hyp_next_deadline(uint64);
void            hyp_sgei(void);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))
