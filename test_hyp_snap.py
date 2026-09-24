#
# the `snap' suite: saving a machine, and starting it again (§9).
#
# What a save has to be is boring: a machine written down, and the same
# machine again afterwards. So every test here proves continuity rather
# than mere survival -- the restored guest has to have what the saved one
# had and not have booted in between, which is the whole difference
# between a save and a restart.
#
import os
import re
import test_hyp_guest as g

HERE = os.path.dirname(os.path.abspath(__file__))

ATONCE = g.ATONCE


def run(q, line, timeout=60):
    m = q.mark()
    q.cmd_(line, timeout=timeout)
    return q.output_since(m)


def guest_say(q, line, timeout=None):
    """type a line at the *guest's* console (attached) and return what it
    said. The guest's shell prompt is the same `$ ' as the host's, which
    is why every one of these is anchored on something the guest prints."""
    q.send(line + "\n")
    m = q.expect(r"\$ ", timeout or ATONCE)
    return m


def t_snap_save_restore(QEMU, intc, cpus, log):
    """a guest saved to a file and started again from it: it goes on from
    where it stopped, with what it had in memory still there."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        vm = g.start_guest(q, "-n g1 -m 16 -c 1", 120)
        # something to remember it by, in the guest's own memory: a file
        # in its tmpfs, which is gone if it reboots and there if it does
        # not. (Its disk would survive a reboot, which would prove
        # nothing.)
        q.send("echo the-marker > /tmp/keep\n")
        q.expect(r"\$ ", ATONCE)
        q.send("cat /tmp/keep\n")
        q.expect(r"the-marker", ATONCE)
        q.expect(r"\$ ", ATONCE)
        g.detach(q)

        out = run(q, "vmm save g1 /g1.vst", 120)
        m = re.search(r"saved to /g1.vst \((\d+) KB\)", out)
        assert m, out
        kb = int(m.group(1))
        # sparse: a 16 MB guest that has touched a few megabytes writes a
        # few megabytes, not sixteen
        assert 200 < kb < 16 * 1024, out
        # and the machine it was written from is still running
        out = run(q, "vmm ls", 30)
        assert re.search(r"0\s+g1\s+running", out), out

        # the same machine, from the file. (The original has to go first:
        # two machines may not have the same disk image open, which is
        # the point of `vmm clone'.)
        run(q, "vmm kill g1", 60)
        out = run(q, "vmm restore /g1.vst -n g2 -d", 120)
        assert "restored from /g1.vst" in out, out
        out = run(q, "vmm ls", 30)
        assert re.search(r"\d+\s+g2\s+running", out), out

        # it did not boot: what it says is what a shell says, and the
        # marker is still in its memory
        g.attach(q, "g2")
        q.send("\n")
        q.expect(r"\$ ", 30)
        q.send("cat /tmp/keep\n")
        q.expect(r"the-marker", 30)
        q.expect(r"\$ ", ATONCE)
        # and nothing in between said it was booting
        m = q.mark()
        q.send("uname -a\n")
        q.expect(r"xv6", ATONCE)
        q.expect(r"\$ ", ATONCE)
        out = q.output_since(m)
        assert "kernel is booting" not in out, out
        g.detach(q)
        run(q, "vmm kill g2", 60)
        print("    a machine written down and started again, still itself")
    finally:
        q.close()


def t_snap_refused(QEMU, intc, cpus, log):
    """what cannot be saved says so: a machine whose disk is a device
    rather than an image, and a file that is not a saved machine."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        # a file that is not one of ours
        run(q, "echo not-a-machine > /junk.vst", 20)
        out = run(q, "vmm restore /junk.vst", 60)
        assert "restoring" in out and (
            "invalid" in out.lower() or "argument" in out.lower()), out
        # and a name that is not a file
        out = run(q, "vmm restore /nothing.vst", 60)
        assert "restoring" in out, out
        # a machine given a whole block device: the save refuses rather
        # than writing a file that names a disk it cannot promise
        out = run(q, "losetup /dev/loop0 /disk1.img", 30)
        vm = None
        out = run(q, "vmm start -n d1 -m 16 -c 1 -f /dev/loop0 -d", 90)
        if "vm " in out:
            out = run(q, "vmm save d1 /d1.vst", 60)
            assert "saving" in out and "supported" in out.lower(), out
            run(q, "vmm kill d1", 60)
        run(q, "losetup -d /dev/loop0", 30)
        print("    what cannot be saved is refused, with a reason")
    finally:
        q.close()


def t_snap_live(QEMU, intc, cpus, log):
    """a snapshot taken while the guest runs: the machine keeps going, and
    the file is the machine as it was when the snapshot began."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        vm = g.start_guest(q, "-n g1 -m 16 -c 1", 120)
        q.send("echo live-marker > /tmp/keep\n")
        q.expect(r"\$ ", ATONCE)
        # something that dirties memory while the snapshot is taken, so
        # that the guest's own writes are what put some of these pages in
        # the file (the fault path, not the walk)
        q.send("grind 2 > /dev/null &\n")
        q.expect(r"\$ ", ATONCE)
        g.detach(q)

        out = run(q, "vmm snapshot g1 /s1.vst", 180)
        m = re.search(r"snapshotted to /s1.vst \((\d+) KB\), still running", out)
        assert m, out
        assert 200 < int(m.group(1)) < 16 * 1024, out
        # the machine it was taken from is still running, and still itself
        out = run(q, "vmm ls", 30)
        assert re.search(r"\d+\s+g1\s+running", out), out
        g.attach(q, "g1")
        q.send("\n")
        q.expect(r"\$ ", 30)
        q.send("cat /tmp/keep\n")
        q.expect(r"live-marker", 30)
        q.expect(r"\$ ", ATONCE)
        g.detach(q)

        # and the file is a machine: the same one, at the moment the
        # snapshot began
        run(q, "vmm kill g1", 60)
        out = run(q, "vmm restore /s1.vst -n g2 -d", 120)
        assert "restored from /s1.vst" in out, out
        g.attach(q, "g2")
        q.send("\n")
        q.expect(r"\$ ", 30)
        q.send("cat /tmp/keep\n")
        q.expect(r"live-marker", 30)
        q.expect(r"\$ ", ATONCE)
        m = q.mark()
        q.send("echo still-here\n")
        q.expect(r"still-here", ATONCE)
        q.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in q.output_since(m), q.output_since(m)
        g.detach(q)
        run(q, "vmm kill g2", 60)
        print("    a snapshot of a running machine, restored and still itself")
    finally:
        q.close()


def t_snap_clone(QEMU, intc, cpus, log):
    """a clone: the same machine again, sharing its pages until one of the
    two writes. Both keep running, and neither sees the other's writes."""
    q = g.boot_host(QEMU, intc, cpus, log)
    try:
        vm = g.start_guest(q, "-n g1 -m 16 -c 1", 120)
        q.send("echo from-the-original > /tmp/keep\n")
        q.expect(r"\$ ", ATONCE)
        g.detach(q)

        # the clone's disk is its own: two machines with one image open
        # are two kernels writing one file system. A copy here is what an
        # overlay over the original's image would give it.
        run(q, "cp /disk0.img /c.img", 60)
        out = run(q, "vmm clone g1 g2 -f /c.img -d", 120)
        assert "cloned from g1" in out, out
        assert "shared" in out, out
        out = run(q, "vmm ls", 30)
        assert re.search(r"0\s+g1\s+running", out), out
        assert re.search(r"1\s+g2\s+running", out), out
        # the clone holds the original's memory without a copy of it
        out = run(q, "vmm info 1", 60)
        m = re.search(r"\((\d+) MB resident\)", out)
        assert m and int(m.group(1)) > 0, out

        # it has what the original had
        g.attach(q, "g2")
        q.send("\n")
        q.expect(r"\$ ", 30)
        q.send("cat /tmp/keep\n")
        q.expect(r"from-the-original", 30)
        q.expect(r"\$ ", ATONCE)
        # and what it writes is its own: the shared page is copied at the
        # write, which is what the two of them sharing pages has to mean
        q.send("echo only-in-the-clone > /tmp/mine\n")
        q.expect(r"\$ ", ATONCE)
        g.detach(q)
        g.attach(q, "g1")
        q.send("\n")
        q.expect(r"\$ ", 30)
        m = q.mark()
        q.send("cat /tmp/mine\n")
        q.expect(r"\$ ", 30)
        out = q.output_since(m)
        assert "only-in-the-clone" not in out, out
        q.send("cat /tmp/keep\n")
        q.expect(r"from-the-original", 30)
        q.expect(r"\$ ", ATONCE)
        g.detach(q)

        run(q, "vmm kill g2", 60)
        run(q, "vmm kill g1", 60)
        print("    a clone shares its original's pages, and copies at a write")
    finally:
        q.close()


def t_migrate(QEMU, intc, cpus, log):
    """a machine moved from one host to another over a connection (§9).

    The two hosts are joined to each other by a unix-socket wire on the
    laptop's file system -- not a port: nothing listens on the network, and
    the only thing at the other end is the other emulated machine. Neither
    host has a way out to anywhere else, which is why they configure their
    addresses by hand."""
    import os
    # names of this run's own: the two migration tests run side by side on
    # the build box, and one wire and one pair of disks between four
    # machines is two tests failing for a reason that is not theirs.
    tag = "mig%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = QEMU(intc=intc, cpus=cpus, log=log, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc=intc, cpus=cpus, log=log, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)

        # no DHCP on a wire with nobody on it but the two of them
        out = run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        assert "not" not in out.lower() or "error" not in out.lower(), out
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        out = run(a, "ifconfig", 30)
        assert "10.7.0.1" in out and "(static)" in out, out
        # and they can see each other, which is the whole of their network
        out = run(a, "ping 10.7.0.2 2", 60)
        assert "2 packets transmitted, 2 received" in out, out

        # a machine on A, with something of its own in memory
        vm = g.start_guest(a, "-n m1 -m 16 -c 1", 120)
        a.send("echo crossed-over > /tmp/keep\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)

        # B waits for it; A sends it
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate m1 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        # the memory went over while the guest was running, and the guest
        # was stopped only for what it wrote during the last round (§9)
        assert re.search(r"pre-copy round 1: \d+ KB", out), out
        assert re.search(r"stopped for \d+ page", out), out
        # and it is gone from A. Destroying a machine is not instant -- its
        # vcpus have to come out of the guest first -- so this is asked for
        # a few times rather than once.
        for _ in range(10):
            out = run(a, "vmm ls", 30)
            if "no vms" in out:
                break
            run(a, "sleep 1", 20)
        assert "no vms" in out, out
        # and arrived on B, going on from where it stopped
        b.expect(r"arrived: 1 vcpu", 120)
        # the accept was put in the background, so its news arrives in the
        # middle of a prompt that was printed long before: ask for a fresh
        # one rather than waiting for a prompt nobody is going to print.
        b.send("\n")
        b.expect(r"\$ ", 30)
        out = run(b, "vmm ls", 30)
        assert re.search(r"\d+\s+m1\s+running", out), out
        g.attach(b, "m1")
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("cat /tmp/keep\n")
        b.expect(r"crossed-over", 60)
        b.expect(r"\$ ", ATONCE)
        m = b.mark()
        b.send("echo still-here\n")
        b.expect(r"still-here", ATONCE)
        b.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in b.output_since(m), b.output_since(m)
        g.detach(b)
        run(b, "vmm kill m1", 60)
        print("    a machine crossed from one host to another and went on")
    finally:
        if b:
            b.close()
        a.close()
        for f in (link, os.path.join(HERE, tag + "-a.img"),
                  os.path.join(HERE, tag + "-b.img")):
            try:
                os.unlink(f)
            except OSError:
                pass


def t_migrate_busy(QEMU, intc, cpus, log):
    """the same, with a guest that is dirtying memory as fast as it can.

    A guest filling a file in its tmpfs is writing megabytes of its own
    memory a second, which is what makes a pre-copy take round after
    round: what the first round sent is out of date before it has
    finished. §9 asks for as few rounds as it takes and for the guest to
    be stopped only at the end, and both are what this checks.

    What it does *not* do is write to its disk. A migration carries the
    machine's memory and the name of its image, not the image: the two
    hosts here have identical copies, and a guest that had written to one
    of them would arrive beside a disk that did not have those writes.
    Carrying the disk as well is not done and is named in §15."""
    import os
    # names of this run's own: the two migration tests run side by side on
    # the build box, and one wire and one pair of disks between four
    # machines is two tests failing for a reason that is not theirs.
    tag = "mig%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = QEMU(intc=intc, cpus=cpus, log=log, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc=intc, cpus=cpus, log=log, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        b.send("vmm accept 9000 -n m2 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)

        # a guest that has finished booting, so that its disk is quiet,
        # and is then told to fill sixteen megabytes of its tmpfs: that is
        # its own memory, dirtied as fast as the emulator will go.
        g.start_guest(a, "-n m2 -m 48 -c 1", 120)
        a.send("dd if=/dev/zero of=/tmp/z bs=4096 count=4000 &\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        out = run(a, "vmm migrate m2 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        # the memory crossed while the guest ran, and what it was stopped
        # for is a fraction of that -- which is the whole point of §9's
        # rounds. How many rounds it takes depends on how fast the guest
        # dirties memory against how fast the wire carries it, so the test
        # asks for a short outage rather than a particular number.
        rounds = len(re.findall(r"pre-copy round \d+:", out))
        assert rounds >= 1, out
        first = int(re.search(r"pre-copy round 1: (\d+) KB", out).group(1))
        last = int(re.search(r"stopped for \d+ page\(s\), (\d+) KB", out).group(1))
        assert first > 1024, out          # a megabyte at least went over live
        assert last * 4 < first, out      # and the outage is a fraction of it
        # and it is going on over there, from where it was: the shell it
        # had is the shell it has, and it did not start again
        b.expect(r"arrived: 1 vcpu", 120)
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "m2")
        b.send("\n")
        b.expect(r"\$ ", 60)
        m = b.mark()
        b.send("echo it-lived\n")
        b.expect(r"it-lived", 60)
        b.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in b.output_since(m), b.output_since(m)
        g.detach(b)
        run(b, "vmm kill m2", 60)
        print("    a machine dirtying memory crossed in %d rounds: %d KB "
              "with it running, %d KB with it stopped" % (rounds, first, last))
    finally:
        if b:
            b.close()
        a.close()
        for f in (link, os.path.join(HERE, tag + "-a.img"),
                  os.path.join(HERE, tag + "-b.img")):
            try:
                os.unlink(f)
            except OSError:
                pass


def tests(modes, cpus, log, QEMU):
    m = modes[0]
    return [
        ("snap-save-restore", lambda: t_snap_save_restore(QEMU, m, cpus, log)),
        ("snap-live", lambda: t_snap_live(QEMU, m, cpus, log)),
        ("snap-clone", lambda: t_snap_clone(QEMU, m, cpus, log)),
        ("snap-refused", lambda: t_snap_refused(QEMU, m, cpus, log)),
        ("snap-migrate", lambda: t_migrate(QEMU, m, cpus, log)),
        ("snap-migrate-busy", lambda: t_migrate_busy(QEMU, m, cpus, log)),
    ]
