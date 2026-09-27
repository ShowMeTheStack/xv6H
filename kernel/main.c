#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "nsproxy.h"
#include "cgroup.h"
#include "platform.h"
#include "intc.h"
#include "unixsock.h"

volatile static int started = 0;

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if (cpuid() == 0) {
    consoleinit();
    devnullinit();
    loginit();
    printkinit();
    // before the first line of output, not after: what a guest prints
    // while booting is most of what it prints at all, and each character
    // of it on the 16550 is two exits. The device tree has already been
    // read (plat is what the line below reports), and this needs nothing
    // else -- its rings are static exactly so that it can run here.
    machine_init();     // the revision lock, before any other hart can arrive
    virtio_con_init();
    printk("\n");
    printk("xv6 kernel is booting\n");
    printk("\n");
    printk("platform: %s, ram %ld MB, %s paging, %d cpus, intc %s, %d virtio slots%s\n",
           plat.is_guest ? "xv6h guest" : "qemu virt",
           plat.ram_size / (1024 * 1024), plat.mmu_levels == 4 ? "sv48" : "sv39",
           plat.ncpu,
           plat.intc_kind == INTC_PLIC ? "plic" : plat.intc_kind == INTC_APLIC ? "aplic" : "aplic-msi+imsic",
           plat.nvirtio, plat.has_h ? ", hypervisor ext" : "");
    if (plat.has_v)
      printk("vector extension: vlen %ld bits\n", plat.vlenb * 8);
    kinit();            // physical page allocator
    slabinit();         // object caches
    mminit();           // address spaces
    siginit();          // signal state caches
    kvminit();          // create kernel page table
    kvminithart();      // turn on paging
    kstackinit();       // the kernel stack region
    procinit_early();   // command-line settings for processes
    schedinit();        // the run queues, before any task is made
    nsinit();      // the root namespaces
    cginit();      // and the root control group
    procinit();         // processes
    trapinit();         // trap vectors
    trapinithart();     // install kernel trap vector
    intc_init();        // set up interrupt controller
    intc_inithart();    // ask it for device interrupts
    iinit();            // inode table
    fileinit();         // file table
    unixsock_init();    // sockets that never leave the machine
    virtio_disk_init();
    blkinit();
    vfs_init();
    procfs_init();
    devfs_init();
    ramfs_init();
    sysfs_init();
    overlayfs_init();
    loopinit();
    ext4_init();
    rtcinit();          // the real-time clock
    randominit();       // the random generator
    auditinit();        // the record of privileged operations
    auditdevinit();     // and /dev/audit, where a program adds to it
    bridgeinit();       // software switch between host, guests and the NIC
    netinit();          // network device and stack
    ksm_init();         // same-page merging between guests (off until asked)
    hypinit();          // hypervisor (if the H extension is present)
    userinit();         // first user process
    // after userinit, because its thread belongs to init like every
    // other kernel thread. only a guest has a balloon; on a machine
    // without one the probe finds nothing and that is the whole of it.
    virtio_balloon_init();
    rbtest();           // the scheduler's tree, before anything uses it
    kthread_selftest(); // two kernel threads take turns, then print

    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0)
      ;

    printk("hart %d starting\n", cpuid());
    kvminithart();   // turn on paging
    trapinithart();  // install kernel trap vector
    intc_inithart(); // ask interrupt controller for device interrupts
    hypinithart();   // hypervisor CSRs
  }

  scheduler();
}
