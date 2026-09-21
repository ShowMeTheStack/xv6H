#
# Linux guest tests for test-hyp.py. they need linux/Image and
# linux/linux.img (built on a Linux box by linux/build.sh and fetched with
# linux/fetch.sh); test-hyp.py rebuilds fs.img with them when present.
#
import os, re, time
from test_hyp_guest import boot_host, start_guest, detach, attach, vm_ls, kill_vm, host_stats

LINUX_ARGS = "-k /Image -r 0x80200000 -m 256 -a console=ttyS0 earlycon=sbi"


def available(here):
    return os.path.exists(os.path.join(here, "linux", "Image")) and \
        os.path.exists(os.path.join(here, "linux", "linux.img"))


def start_linux(q, extra="-f none", name="linux", timeout=90):
    q.send("vmm start -n %s %s %s\n" % (name, LINUX_ARGS, extra))
    m = q.expect(r"vm (\d+) %s: " % name, 20)
    vmid = int(m.group(1))
    q.expect(r"Linux version 6\.", timeout)
    q.expect(r"Run /init as init process", timeout)
    q.expect(r"xv6h-linux: ready", 60)
    q.expect(r"linux# ", 60)
    return vmid


def linux_cmd(q, cmd, timeout=30):
    """run a shell command in the attached Linux guest and return everything
    printed up to the next prompt (the echoed command included; the guest
    terminal may wrap it)."""
    mark = q.mark()
    q.send(cmd + "\n")
    q.expect(r"linux# ", timeout)
    return q.output_since(mark)


def have_https(q):
    """is the TLS client in this initramfs (linux/build-https.sh)?"""
    return "yes-https" in linux_cmd(q, "test -x /bin/https && echo yes-https")


def poweroff(q):
    q.send("poweroff\n")
    q.expect(r"reboot: Power down", 60)
    q.expect(r"\[vm \d+ has exited\]", 30)
    q.expect(r"\$ ", 10)


def t_linux_boot(QEMU, intc, cpus, log):
    q = boot_host(QEMU, intc, cpus, log)
    try:
        free0, _ = host_stats(q)
        t0 = time.time()
        start_linux(q)
        print("    booted to a shell in %.0fs" % (time.time() - t0))
        out = linux_cmd(q, "uname -a")
        assert "6.12" in out and "riscv64" in out, out
        out = linux_cmd(q, "cat /etc/marker")
        assert "marker-in-initramfs" in out, out
        out = linux_cmd(q, "cat /proc/cpuinfo")
        assert "rv64imafdcv" in out, out
        out = linux_cmd(q, "cat /proc/interrupts")
        assert "ttyS0" in out and "riscv-timer" in out, out
        # timer accuracy through the hypervisor (idle wake-ups)
        out = linux_cmd(q, "time sleep 1", 30)
        m = re.search(r"real\s+0m\s*([\d.]+)s", out)
        assert m and 0.9 <= float(m.group(1)) <= 1.5, out
        print("    sleep 1 took %ss" % m.group(1))
        # floating point and vectors in the RVA23 userland
        out = linux_cmd(q, "echo 'scale=6; 4*a(1)' | bc 2>/dev/null || echo nobc")
        out = linux_cmd(q, "date && uptime && free && ls -l /bin | head -3")
        assert "load average" in out, out
        # console input: a here-document typed at the guest
        out = linux_cmd(q, "echo typed-into-linux | tr a-z A-Z")
        assert "TYPED-INTO-LINUX" in out, out
        poweroff(q)
        assert "no vms" in vm_ls(q)
        # the guest's memory comes back at once; what it wrote to its
        # disk image is still dirty in the host's block cache, and those
        # pages are only reclaimable once kflushd has written them out.
        free1 = wait_free(q, free0, timeout=20)
        # a megabyte of slack, and it is the report's own resolution:
        # "free guest memory" is printed in megabytes, so two readings a
        # page apart can differ by one. Booting Linux also reads tens of
        # megabytes of kernel image through the block cache, and the
        # object cache behind those cached blocks keeps a partly used
        # slab or two. The guest's own memory is what this checks, and
        # 128 MB of it either came back or did not.
        assert free1 >= free0 - 1, (free0, free1)
    finally:
        q.close()


def wait_free(q, free0, timeout=20):
    """free memory comes back over a moment, not at once: a killed guest's
    pages return when its last vcpu has left, and the blocks it wrote are
    reclaimable only after the writeback thread has put them on the disk."""
    end = time.time() + timeout
    while True:
        free1, _ = host_stats(q)
        if free1 >= free0 - 1 or time.time() > end:
            return free1
        time.sleep(0.5)


def t_linux_disk_smp(QEMU, intc, cpus, log, extra=""):
    q = boot_host(QEMU, intc, cpus, log)
    try:
        start_linux(q, "-f /linux.img -c 2 -H " + extra)
        out = linux_cmd(q, "nproc")
        assert re.search(r"\n2\r?\n", out), out
        out = linux_cmd(q, "cat /proc/interrupts")
        assert "CPU1" in out and "Rescheduling" in out, out
        out = linux_cmd(q, "mount /dev/vda /mnt && cat /mnt/marker")
        assert "marker-on-virtio-disk" in out, out
        out = linux_cmd(q, "echo written-by-linux > /mnt/note && sync && cat /mnt/note")
        assert "written-by-linux" in out, out
        # both cpus do work: two busy loops finish in about the time of one
        # long enough that the ratio means something: at 20000 the loop
        # took half a second on a busy build box and the noise was
        # larger than the parallelism being measured.
        loop = "i=0; while [ $i -lt 60000 ]; do i=$((i+1)); done"
        # twice, and the slower of the two is the baseline: the box
        # this runs on may be running five other suites, and a single
        # measurement catches whatever it was doing at the time.
        one = 0
        for _ in range(2):
            out = linux_cmd(q, "time sh -c '%s'" % loop, 120)
            m = re.search(r"real\s+0m\s*([\d.]+)s", out)
            assert m, out
            one = max(one, float(m.group(1)))
        out = linux_cmd(q, "time sh -c '(%s) & (%s); wait'" % (loop, loop), 120)
        m = re.search(r"real\s+0m\s*([\d.]+)s", out)
        assert m, out
        two = float(m.group(1))
        print("    one loop %.2fs, two loops in parallel %.2fs" % (one, two))
        assert two < 1.7 * one, (one, two)
        linux_cmd(q, "umount /mnt")
        poweroff(q)
        # persistence: the note survives in /linux.img inside the host fs
        start_linux(q, "-f /linux.img")
        out = linux_cmd(q, "mount /dev/vda /mnt && cat /mnt/note")
        assert "written-by-linux" in out, out
        poweroff(q)
    finally:
        q.close()


def t_linux_with_xv6(QEMU, intc, cpus, log):
    """Linux and xv6 guests at the same time on one host."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        start_linux(q, "-f none -c 2 -H")
        linux_cmd(q, "(while true; do i=$((i+1)); done) &")
        q.send("\x1d")
        q.expect(r"\[detached from vm \d+\]", 10)
        q.expect(r"\$ ", 10)
        vmid = start_guest(q, "-n xv6 -c 2 -f /disk0.img")
        m = q.mark()
        q.send("usertests -q\n")
        q.expect(r"ALL TESTS PASSED|FAILED|panic", 1500)
        assert "ALL TESTS PASSED" in q.output_since(m)
        q.expect(r"\$ ", 30)
        detach(q)
        attach(q, "linux")
        q.send("\n")
        q.expect(r"linux# ", 20)
        out = linux_cmd(q, "echo still-alive")
        assert re.search(r"\nstill-alive", out), out
        q.send("\x1d")
        q.expect(r"\[detached", 10)
        q.expect(r"\$ ", 10)
        q.send("vmm kill --all\n")
        q.expect(r"vm \d+ killed", 30)
        q.expect(r"vm \d+ killed", 30)
        q.expect(r"\$ ", 10)
        assert "no vms" in vm_ls(q)
    finally:
        q.close()


def tests(modes, cpus, log, QEMU):
    t = []
    for m in modes:
        t.append(("linux-boot-%s" % m, lambda m=m: t_linux_boot(QEMU, m, cpus, log)))
        t.append(("linux-disk-smp-%s" % m, lambda m=m: t_linux_disk_smp(QEMU, m, cpus, log)))
        if m == "imsic":
            t.append(("linux-disk-smp-imsic-soft", lambda m=m: t_linux_disk_smp(QEMU, m, cpus, log, "-s")))
    t.append(("linux-with-xv6-plic", lambda: t_linux_with_xv6(QEMU, "plic", cpus, log)))
    return t
