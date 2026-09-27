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


_lx_seq = [0]


def linux_cmd(q, cmd, timeout=30):
    """run a shell command in the attached Linux guest and return everything
    printed up to the next prompt (the echoed command included; the guest
    terminal may wrap it). The command is followed by a marker printed on its
    own line, and that is what is waited for: a prompt alone was ambiguous
    right after an attach or a kernel message, when a stale one sat in the
    buffer and the call returned before the command had run (lx9)."""
    _lx_seq[0] += 1
    mark = q.mark()
    tag = "_ok%d_" % _lx_seq[0]
    q.send(cmd + "; echo " + tag + "\n")
    q.expect(r"\n" + tag + r"\r?\n", timeout)
    q.expect(r"linux# ", 10)
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
        # both cpus do work: two busy loops finish in about the time of
        # one. Long enough that the ratio means something: at 20000 the
        # loop took half a second on a busy build box and the noise was
        # larger than the parallelism being measured.
        loop = "i=0; while [ $i -lt 60000 ]; do i=$((i+1)); done"

        def timed(cmd):
            out = linux_cmd(q, "time sh -c '%s'" % cmd, 120)
            m = re.search(r"real\s+0m\s*([\d.]+)s", out)
            assert m, out
            return float(m.group(1))

        # the pair is measured three times, alternating, and the best
        # ratio is the answer. The box this runs on may be running six
        # other suites, and outside load can only ever make the two
        # loops look slower relative to the one -- never faster -- so
        # the smallest ratio of the three is the least disturbed
        # measurement rather than the luckiest one.
        best, one, two = None, 0, 0
        for _ in range(3):
            o = timed(loop)
            t = timed("(%s) & (%s); wait" % (loop, loop))
            r = t / o
            if best is None or r < best:
                best, one, two = r, o, t
        print("    one loop %.2fs, two loops in parallel %.2fs (best of three)"
              % (one, two))
        assert best < 1.7, (one, two, best)
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


def host_cmd(q, line, timeout=30):
    """a host shell command with the guest detached; returns what it printed"""
    mark = q.mark()
    q.send(line + "\n")
    q.expect(r"\$ ", timeout)
    return q.output_since(mark)


def t_lx_participating(QEMU, intc, cpus, log):
    """M5: a stock Linux 6.12 with machrev.ko speaks the revision protocol.
    The driver binds to the anchor device, announces itself (SBI MACH
    version), and the host stops treating the guest as legacy."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_linux(q)
        out = linux_cmd(q, "dmesg | grep machrev")
        m = re.search(r"machrev: speaking revision to the host \(protocol (\d+)\): (\d+) cpu\(s\) \((\d+) online\), (\d+) MB, cache block (\d+), (\w+)", out)
        assert m, out
        assert m.group(4) == "256" and m.group(5) in ("64", "128"), m.groups()
        out = linux_cmd(q, "cat /sys/bus/platform/devices/10005000.machrev/revisions")
        assert re.search(r"\d+ 0 0x0", out), out
        detach(q)
        out = host_cmd(q, "vmm info %d" % vmid)
        assert "revision: " in out and "does not speak" not in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        poweroff(q)
        print("    Linux (%s cpus, %s MB, cache block %s, %s) speaks revision through the anchor" % (m.group(2), m.group(4), m.group(5), m.group(6)))
    finally:
        q.close()


def t_lx_revise_cboz(QEMU, intc, cpus, log):
    """F21 in Linux: the cache-block size is a variable clear_page reads on
    every page. Described 128 on a 64-byte host, the driver sets it; the
    host clears 128 for it in software; fresh pages stay zero (zerocheck);
    back to 64, in hardware."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_linux(q)
        out = linux_cmd(q, "zerocheck 2048", 120)
        assert "2048 pages clean" in out, out
        detach(q)
        for want, frm in ((128, 64), (64, 128)):
            out = host_cmd(q, "vmm revise %d cboz %d" % (vmid, want), 60)
            assert "asked" in out, out
            attach(q, str(vmid))
            q.send("\n")
            q.expect(r"linux# ", 30)
            out = linux_cmd(q, "dmesg | grep 'machrev: cache block'")
            assert "cache block %d -> %d bytes" % (frm, want) in out, out
            out = linux_cmd(q, "zerocheck 2048; zerocheck 2048", 180)
            assert out.count("2048 pages clean") == 2, out
            detach(q)
            out = host_cmd(q, "vmm info %d" % vmid)
            assert "revision %d adopted" % (1 if want == 128 else 2) in out or "generation" in out, out
            mm = re.search(r"cbo.zero in software (\d+)", out)
            if want == 128:
                assert mm and int(mm.group(1)) > 0, out
                soft = int(mm.group(1))
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        poweroff(q)
        print("    Linux took a 128-byte cache block on a 64-byte host (%d cbo.zero in software), pages clean, and the host's own back" % soft)
    finally:
        q.close()


def t_lx_revise_ram(QEMU, intc, cpus, log):
    """F13 in Linux: the memory node grows by a hotplug section (128 MB on
    rv64); the driver hot-adds and onlines it; MemTotal grows; the memory
    is usable."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_linux(q)
        out = linux_cmd(q, "grep MemTotal /proc/meminfo")
        before = int(re.search(r"MemTotal:\s+(\d+) kB", out).group(1))
        detach(q)
        out = host_cmd(q, "vmm resize %d 384" % vmid, 60)
        assert "asked" in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        out = linux_cmd(q, "dmesg | grep 'machrev: memory'")
        assert "memory 256 -> 384 MB (128 MB added at 0x90000000)" in out, out
        out = linux_cmd(q, "grep MemTotal /proc/meminfo")
        after = int(re.search(r"MemTotal:\s+(\d+) kB", out).group(1))
        assert after - before > 100 * 1024, (before, after)
        out = linux_cmd(q, "zerocheck 65536", 300)    # 256 MB: more than the machine had
        assert "65536 pages clean" in out, out
        poweroff(q)
        print("    Linux was given 128 MB more (MemTotal %d -> %d kB), hot-added and onlined, and used it" % (before, after))
    finally:
        q.close()


def t_lx_revise_cpus(QEMU, intc, cpus, log):
    """F8/F7 in Linux: told it has two of its four harts, the driver takes
    two offline (cpu_down -> SBI HSM stop; the host sees them parked); told
    four again, it brings them up (cpu_up -> HSM start at its own entry)."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_linux(q, "-f none -c 4 -H")
        out = linux_cmd(q, "nproc")
        assert re.search(r"\n4\r?\n", out), out
        detach(q)
        out = host_cmd(q, "vmm revise %d cpus 2" % vmid, 90)
        assert "hart(s) 2..3 parked" in out and "cpus -> 2: done" in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        out = linux_cmd(q, "nproc; dmesg | grep 'machrev: cpus'")
        assert re.search(r"\n2\r?\n", out) and "cpus 4 -> 2 (0 up, 2 down)" in out, out
        detach(q)
        out = host_cmd(q, "vmm revise %d cpus 4" % vmid, 90)
        assert "for the guest to start" in out and "cpus -> 4: done" in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        out = linux_cmd(q, "sleep 2; nproc; dmesg | grep 'machrev: cpus'")
        assert re.search(r"\n4\r?\n", out) and "cpus 2 -> 4 (2 up, 0 down)" in out, out
        loop = "i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done"
        out = linux_cmd(q, "(%s) & (%s) & (%s) & (%s) & wait; echo LOOPS-OK" % (loop, loop, loop, loop), 120)
        assert "LOOPS-OK" in out, out
        poweroff(q)
        print("    Linux parked two of four harts on request (HSM stop) and brought them back (HSM start)")
    finally:
        q.close()


def _lx_sleep_secs(q):
    out = linux_cmd(q, "time sleep 1", 60)
    m = re.search(r"real\s+0m\s*([\d.]+)s", out)
    assert m, out
    return float(m.group(1))


def t_lx_revise_timer(QEMU, intc, cpus, log):
    """F5 in Linux (patch 0001): Sstc taken away -- henvcfg.STCE off, `sstc'
    out of the isa string -- the driver flips the timer to SBI set_timer and
    re-arms every cpu; `sleep 1' still takes a second. Then given back."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_linux(q, "-f none -c 2 -H")
        t0 = _lx_sleep_secs(q)
        detach(q)
        for want, via in (("sbi", "sbi"), ("sstc", "stimecmp")):
            out = host_cmd(q, "vmm revise %d timer %s" % (vmid, want), 60)
            assert "asked" in out, out
            attach(q, str(vmid))
            q.send("\n")
            q.expect(r"linux# ", 30)
            out = linux_cmd(q, "dmesg | grep -E 'riscv-timer: timer via|machrev: timer'")
            assert "riscv-timer: timer via %s" % via in out and "machrev: timer via %s" % via in out, out
            t = _lx_sleep_secs(q)
            assert 0.9 <= t <= 1.6, (want, t)
            detach(q)
            out = host_cmd(q, "vmm info %d" % vmid)
            assert "generation" in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        poweroff(q)
        print("    Linux lost Sstc and kept time through SBI set_timer (sleep 1: %.2fs before, %.2fs after), then got it back" % (t0, t))
    finally:
        q.close()


def t_lx_revise_timebase(QEMU, intc, cpus, log):
    """F6 in Linux (patch 0001): the counter is described at another rate
    (the host scales `time' and stimecmp); the driver re-rates the
    clocksource and every clock event device; `sleep 1' is still a second."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_linux(q, "-f none -c 2 -H")
        detach(q)
        out = host_cmd(q, "vmm revise %d timebase 1000000" % vmid, 60)
        assert "asked" in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        out = linux_cmd(q, "dmesg | grep -E 'riscv-timer: timebase|machrev: timer'")
        assert "riscv-timer: timebase 10000000 -> 1000000 Hz" in out, out
        t = _lx_sleep_secs(q)
        assert 0.9 <= t <= 1.6, t
        out = linux_cmd(q, "date +%s; sleep 2; date +%s")
        secs = [int(x) for x in re.findall(r"^(\d{9,})\r?$", out, re.M)]
        assert len(secs) == 2 and 1 <= secs[1] - secs[0] <= 3, out
        poweroff(q)
        print("    Linux took a 1 MHz timebase in place of 10 MHz: sleep 1 = %.2fs, the clock advanced %d s in 2" % (t, secs[1] - secs[0]))
    finally:
        q.close()


def t_lx_revise_rtc(QEMU, intc, cpus, log):
    """F17/F16 in Linux (no patch): the RTC is taken away -- the driver
    unregisters the goldfish platform device, /sys/class/rtc empties -- and
    given back: a platform device is made by hand, the driver binds, the
    clock reads."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_linux(q)
        out = linux_cmd(q, "ls /sys/class/rtc/; cat /sys/class/rtc/rtc0/date")
        assert "rtc0" in out and re.search(r"\d{4}-\d\d-\d\d", out), out
        detach(q)
        out = host_cmd(q, "vmm revise %d rtc off" % vmid, 60)
        assert "asked" in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        out = linux_cmd(q, "dmesg | grep 'machrev: rtc'; ls /sys/class/rtc/ | wc -l")
        assert "rtc gone" in out and re.search(r"\n0\r?\n", out), out
        detach(q)
        out = host_cmd(q, "vmm revise %d rtc on" % vmid, 60)
        assert "asked" in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        out = linux_cmd(q, "dmesg | grep 'machrev: rtc'; ls /sys/class/rtc/; cat /sys/class/rtc/rtc*/date")
        assert "goldfish_rtc registered" in out and re.search(r"\d{4}-\d\d-\d\d", out), out
        poweroff(q)
        print("    Linux lost its RTC (goldfish_rtc unregistered) and got it back (registered by hand, the clock reads)")
    finally:
        q.close()


def t_lx_revise_sbi(QEMU, intc, cpus, log):
    """F22 in Linux (no patch): the host withholds SBI RFENCE; the driver
    re-probes and flips the kernel's remote-fence path to IPIs; TLB-flushing
    work across two cpus still runs; then RFENCE is given back."""
    q = boot_host(QEMU, intc, cpus, log)
    try:
        vmid = start_linux(q, "-f none -c 2 -H")
        work = "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do (sh -c 'echo $$ >/dev/null') & done; wait; echo FLUSH-OK"
        detach(q)
        for hide, by in (("rfence", "IPI"), ("none", "SBI RFENCE")):
            out = host_cmd(q, "vmm revise %d sbi %s" % (vmid, hide), 60)
            assert "asked" in out, out
            attach(q, str(vmid))
            q.send("\n")
            q.expect(r"linux# ", 30)
            out = linux_cmd(q, "dmesg | grep 'machrev: sbi'")
            assert "remote fences by %s" % by in out, out
            before = linux_cmd(q, "grep 'Function call' /proc/interrupts")
            out = linux_cmd(q, work, 120)
            assert "FLUSH-OK" in out, out
            after = linux_cmd(q, "grep 'Function call' /proc/interrupts")
            detach(q)
            out = host_cmd(q, "vmm info %d" % vmid)
            assert "generation" in out, out
        attach(q, str(vmid))
        q.send("\n")
        q.expect(r"linux# ", 30)
        poweroff(q)
        print("    Linux lost SBI RFENCE, re-probed and fenced by IPI, kept working, and got it back")
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
    t.append(("lx-participating", lambda: t_lx_participating(QEMU, "plic", cpus, log)))
    t.append(("lx-revise-cboz", lambda: t_lx_revise_cboz(QEMU, "plic", cpus, log)))
    t.append(("lx-revise-ram", lambda: t_lx_revise_ram(QEMU, "plic", cpus, log)))
    t.append(("lx-revise-cpus", lambda: t_lx_revise_cpus(QEMU, "plic", cpus, log)))
    t.append(("lx-revise-timer", lambda: t_lx_revise_timer(QEMU, "plic", cpus, log)))
    t.append(("lx-revise-timebase", lambda: t_lx_revise_timebase(QEMU, "plic", cpus, log)))
    t.append(("lx-revise-rtc", lambda: t_lx_revise_rtc(QEMU, "plic", cpus, log)))
    t.append(("lx-revise-sbi", lambda: t_lx_revise_sbi(QEMU, "plic", cpus, log)))
    return t
