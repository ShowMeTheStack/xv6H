#!/usr/bin/env python3
# The kernel by subsystem, for the paper's size table (tab:sizes).
#   tools/ksizes.py [COMMIT]     (default HEAD)
# Counts every line (wc -l) of kernel/*.c, *.h and *.S at COMMIT and sums them by the map below.
# A file not in the map is reported, so the map cannot silently miss one.
import subprocess, sys

ROWS = [
    ("file systems: ext4 with a journal, tmpfs, procfs, sysfs, devfs, overlay, the log",
     "ext4.h ext4_htree.c ext4_internal.h ext4_journal.c ext4_ondisk.h ext4_read.c ext4_vfs.c "
     "ext4_write.c ext4_xattr.c jbd2.c jbd2.h fs.c fs.h vfs.c vfs.h file.c file.h ramfs.c "
     "procfs.c sysfs.c devfs.c devnull.c overlayfs.c logdev.c xattr.h crc32c.c stat.h fcntl.h"),
    ("block devices and drivers: block layer, loop, ramdisk, virtio, UART, RTC, console",
     "blk.c blk.h blkdev.c blkdev.h loop.c loop.h ramdisk.c virtio.h virtio_disk.c virtio_net.c "
     "virtio_con.c virtio_balloon.c uart.c rtc.c console.c"),
    ("boot, traps, M-mode vector, locks, libraries, headers",
     "entry.S start.c main.c mtrap.c mvec.S trap.c kernelvec.S trampoline.S swtch.S fpu.S "
     "spinlock.c spinlock.h sleeplock.c sleeplock.h string.c printk.c riscv.h defs.h types.h "
     "param.h memlayout.h errno.h elf.h hwinfo.h sha256.c sha256.h canary.c sysconf.h "
     "syscall.c syscall.h exec.c"),
    ("scheduler, processes, control groups, red-black tree",
     "sched.c schedgroup.h proc.c proc.h thread.h cgroup.c cgroup.h rbtree.c rbtree.h rbtest.c "
     "kstack.c futex.c"),
    ("network stack and software switch",
     "net.c net.h tcp.c tcp.h sock.c sock.h sockapi.c socket.h unixsock.c unixsock.h bridge.c "
     "conntrack.c pktcap.c"),
    ("hypervisor core: machines, vCPUs, G-stage, exits, SBI, MMIO decode",
     "hyp.c hyp.h hypcsr.c hypsbi.c hypmmio.c hypvec.S syshyp.c vmm.h"),
    ("virtual devices: UART, disk, NIC, balloon, console, RTC, anchor",
     "vuart.c vvirtio.c vvnet.c vvcon.c vballoon.c vrtc.c vmachrev.c"),
    ("interrupt controllers, host and virtual: PLIC, APLIC, IMSIC, CLINT",
     "plic.c aplic.c aplic.h imsic.c imsic.h intc.c intc.h vplic.c vaplic.c vimsic.c vclint.c vic.c"),
    ("save, snapshot, clone, migrate",
     "vmstate.c vmstate.h"),
    ("machine description: FDT parse and generate, platform, revision",
     "fdt.c fdt.h fdtgen.c platform.c platform.h machine.c"),
    ("memory: buddy allocator, slab, address spaces, mmap, KSM",
     "kalloc.c slab.c slab.h vm.c vm.h mm.c mm.h mman.h page.h ksm.c"),
    ("signals, terminals, time, pipes, the POSIX calls",
     "signal.c signal.h sigtramp.S termios.h time.h posix.h pipe.c ptrace.c ptrace.h "
     "sysfile.c sysproc.c"),
    ("namespaces, containers, system-call filter, audit",
     "nsproxy.c nsproxy.h cgw.c cgw.h sysfilter.c sysfilter.h audit.c cred.h"),
]

def th(n):
    return f"{n:,}".replace(",", "\\,")

def main():
    commit = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
    names = subprocess.run(["git", "ls-tree", "-r", "--name-only", commit, "--", "kernel"],
                           capture_output=True, text=True, check=True).stdout.split()
    files = {n[len("kernel/"):] for n in names if n.endswith((".c", ".h", ".S"))}
    owner = {}
    for row, fs in ROWS:
        for f in fs.split():
            if f in owner:
                sys.exit(f"{f} is in two rows")
            owner[f] = row
    missing = sorted(files - owner.keys())
    if missing:
        sys.exit("not in the map: " + " ".join(missing))
    gone = sorted(set(owner) - files)
    lines = {}
    for f in files:
        blob = subprocess.run(["git", "show", f"{commit}:kernel/{f}"],
                              capture_output=True, check=True).stdout
        lines[f] = blob.count(b"\n")
    tl = tf = 0
    out = []
    for row, fs in ROWS:
        present = [f for f in fs.split() if f in files]
        n = sum(lines[f] for f in present)
        out.append((n, row, len(present)))
        tl += n; tf += len(present)
    for n, row, k in sorted(out, reverse=True):
        print(f"{row} & {th(n)} & {k} \\\\")
    print(f"kernel & {th(tl)} & {tf} \\\\")
    if gone:
        print("% in the map but not at this commit: " + " ".join(gone))

main()
