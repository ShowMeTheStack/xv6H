#!/usr/bin/env python3
# KVM's in-kernel AIA reconfigured under a running guest. A Linux
# guest on the in-kernel AIA (APLIC-MSI and an IMSIC per vCPU) reads its disk in a loop while
# kvmtool moves every vCPU's IMSIC from its hardware guest interrupt file to KVM's software file
# and back (`lkvm revise -n g imsicfile sw|hw`, KVM patch 4), N times each way. Nothing is told to
# the guest: no revision is asked, as for F23 under xv6H.
# After each switch, over a timed window of disk reads (caches dropped first):
#   - KVM's own count of CSR and MMIO exits handled in the kernel, from debugfs, per guest
#     interrupt (the software file traps the guest's IMSIC accesses; the hardware file does not);
#   - the guest's interrupt count, which must keep rising, and the read's throughput;
#   - the guest's oracle: a file written before the first switch read back, a new one written
#     and read back;
#   - no revision in the guest's log (nothing was asked).
#   HOSTKERNEL=~/xv6h-kvm-d5/build-host/arch/riscv/boot/Image \
#   HOSTINITRD=~/xv6h-kvm-d5/host-initramfs.cpio.gz python3 -u t_imsicfile.py N
import sys, os, re, time, statistics
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
h = Host(log=K + "/t_imsicfile%s.host.log" % os.environ.get("TAG", ""))
STATS = "csr_exit_kernel mmio_exit_kernel exits"
per = {"hw": [], "sw": []}; mbs = {"hw": [], "sw": []}; ok = bad = 0
def kvm_stats():
    # KVM's own counters, summed over every VM and vCPU (one VM here), at debugfs's top level
    out = h.run("for s in %s; do echo $s $(cat /sys/kernel/debug/kvm/$s 2>/dev/null || echo 0); done" % STATS, 30)
    return {k: int(v) for k, v in re.findall(r"^(\w+) (\d+)\s*$", out, re.M)}
def guest_irqs(out):
    m = re.search(r"IRQS (\d+)", out or "")
    return int(m.group(1)) if m else None
WINDOW = ("sync; echo 3 > /proc/sys/vm/drop_caches; "
          "a=$(awk '/virtio/ {for (i=2;i<=NF;i++) if ($i ~ /^[0-9]+$/) t+=$i} END{print t+0}' /proc/interrupts); "
          "t0=$(cat /proc/uptime | cut -d' ' -f1); dd if=/dev/vda of=/dev/null bs=4096 count=8192 2>/dev/null; "
          "t1=$(cat /proc/uptime | cut -d' ' -f1); "
          "b=$(awk '/virtio/ {for (i=2;i<=NF;i++) if ($i ~ /^[0-9]+$/) t+=$i} END{print t+0}' /proc/interrupts); "
          "echo IRQS $((b-a)) SECS $t0 $t1")
try:
    print(h.run("mount -t debugfs none /sys/kernel/debug 2>/dev/null; ls /sys/kernel/debug/kvm | head -3", 30))
    h.run(guest_cmd("g", extra="--revise -n mode=none"))
    wait_log(h, "g", r"linux# ", 300)
    print(guest_do(h, "g", "mount /dev/vda /mnt && echo before-the-switches > /mnt/keep && sync && cat /mnt/keep; "
                   "grep -E 'MSI|IMSIC' /proc/interrupts | head -4", 120))
    revs0 = len(re.findall(r"machrev: revision", h.run("cat /tmp/g.log", 30)))
    def window(state):
        s0 = kvm_stats()
        out = guest_do(h, "g", WINDOW, 300)
        s1 = kvm_stats()
        irqs = guest_irqs(out)
        m = re.search(r"SECS ([\d.]+) ([\d.]+)", out or "")
        secs = float(m.group(2)) - float(m.group(1)) if m else None
        d = {k: s1.get(k, 0) - s0.get(k, 0) for k in s0}
        return irqs, secs, d
    # the starting state is the hardware file (KVM's AUTO mode, a free guest file on every hart)
    irqs, secs, d = window("hw")
    print("  start (hardware files): %s interrupts, csr exits %s, mmio exits %s, %.2f s" % (irqs, d.get("csr_exit_kernel"), d.get("mmio_exit_kernel"), secs or -1))
    for i in range(N):
        for val in ("sw", "hw"):
            reply = h.run("lkvm revise -n g imsicfile %s" % val, 60).strip().splitlines()
            reply = next((l.strip() for l in reply if l.strip().startswith("revise:")), " | ".join(reply[-2:]))
            time.sleep(1)
            irqs, secs, d = window(val)
            out = guest_do(h, "g", "cat /mnt/keep; echo round-%d-%s > /mnt/f && sync && cat /mnt/f" % (i, val), 120)
            good = (bool(out) and "before-the-switches" in out and ("round-%d-%s" % (i, val)) in out
                    and irqs is not None and irqs > 0 and reply.startswith("revise: imsicfile"))
            if irqs:
                per[val].append(d.get("csr_exit_kernel", 0) / irqs)
                if secs: mbs[val].append(32.0 / secs)
            ok += good; bad += not good
            print("round %2d %s: %s | %s interrupts, %.2f csr exits and %.2f mmio exits each, %.1f MB/s; oracle %s" % (
                i, val, reply[-110:], irqs, d.get("csr_exit_kernel", 0) / max(irqs or 1, 1),
                d.get("mmio_exit_kernel", 0) / max(irqs or 1, 1), 32.0 / secs if secs else -1, "ok" if good else "FAILED"))
            sys.stdout.flush()
    revs1 = len(re.findall(r"machrev: revision", h.run("cat /tmp/g.log", 30)))
    print("revisions in the guest's log during the switches: %d (nothing was asked)" % (revs1 - revs0))
    for v in ("hw", "sw"):
        if per[v]:
            print("%s files: csr exits per interrupt n=%d median %.2f (min %.2f max %.2f); read MB/s median %.1f" % (
                v, len(per[v]), statistics.median(per[v]), min(per[v]), max(per[v]), statistics.median(mbs[v]) if mbs[v] else -1))
    print("RESULT ok=%d bad=%d unasked=%s" % (ok, bad, "yes" if revs1 == revs0 else "NO"))
finally:
    h.close()
