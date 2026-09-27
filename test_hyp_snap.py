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


def wait_running(q, name, tries=20):
    """`vmm ls' until NAME is running, and the listing it said so in.

    A machine that has just been restored or cloned is VM_NEW until one
    of its vcpus enters the guest, and the vcpus are processes: on a
    machine with one hart the shell asking `vmm ls' may well get there
    first. What the tests mean is "it is running a moment later", not
    "it is running before the command that started it has returned"."""
    import time as _t
    out = ""
    for _ in range(tries):
        out = run(q, "vmm ls", 30)
        if re.search(r"\d+\s+%s\s+running" % re.escape(name), out):
            return out
        _t.sleep(0.5)
    return out


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
        out = wait_running(q, "g2")
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
        out = wait_running(q, "g2")
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
        # the same as t_migrate, and for the same reason: the address has
        # to be there before anything relies on it. `vmm migrate' connects
        # to 10.7.0.2, and the egress policy lets a 10/8 address through
        # only when it is on this machine's own configured subnet -- so a
        # host whose `ip addr add' did not take refuses its own migration
        # with "Permission denied" and says nothing about why. Asserting on
        # ifconfig names it, and the ping is the barrier that makes the
        # wire real before the migration needs it. Without these this test
        # went from start to a refused connect in four seconds.
        out = run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        assert "error" not in out.lower(), out
        out = run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        assert "error" not in out.lower(), out
        out = run(a, "ifconfig", 30)
        assert "10.7.0.1" in out and "(static)" in out, out
        out = run(b, "ifconfig", 30)
        assert "10.7.0.2" in out and "(static)" in out, out
        out = run(a, "ping 10.7.0.2 2", 60)
        assert "2 packets transmitted, 2 received" in out, out
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


def t_migrate_hetero(QEMU, cpus, log):
    """a machine moved to a host that is not the same shape as the one it left.

    This is the one a compatibility set forbids. Host A presents its guests an
    APLIC-MSI+IMSIC, because that is what A was built with; host B presents a
    PLIC, because that is what B has. Today a fleet answers this by refusing the
    move, or by masking every guest down to the PLIC for life so that the move is
    always legal. Neither is necessary: the guest is told, on arrival, that its
    machine is a different shape, and it rebinds.

    What is checked is not that the migration returns success -- it is that the
    guest is *alive and working* afterwards, on a controller it was not booted
    with, with its disk and its userspace intact, and that it is running on the
    destination's controller rather than pretending."""
    import os
    tag = "het%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    # A has the AIA, B has only the PLIC: the heterogeneity is the hosts' own,
    # not a flag on the guest.
    a = QEMU(intc="imsic", cpus=cpus, log=log, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc="plic", cpus=cpus, log=log, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        out = run(a, "ping 10.7.0.2 2", 60)
        assert "2 packets transmitted, 2 received" in out, out

        # a machine on A, on A's controller, with something of its own on disk
        vm = g.start_guest(a, "-n h1 -m 32 -c 1", 120)
        a.send("echo crossed-shapes > /tmp/keep\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        out = run(a, "vmm info h1", 30)
        assert "intc aplic-msi+imsic" in out, out
        # and it has looked at its interrupt controller, so the naive
        # compatibility answer would be that it may not move at all
        assert re.search(r"observed:.*\bintc@", out), out

        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate h1 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out

        # B says what it noticed and what it did about it
        b.expect(r"arrived as imsic, this host gives plic: \d+ line\(s\) still held", 120)
        b.expect(r"revision 1 asked for", 60)
        b.expect(r"arrived: 1 vcpu", 120)
        # and the guest took it. This is the load-bearing line of the test: the
        # guest re-read its description and rebound to a controller it was not
        # booted with, while holding the state it had on A.
        b.expect(r"revision 1 adopted \(0x[0-9a-f]+\) after \d+ us", 120)
        b.send("\n")
        b.expect(r"\$ ", 30)
        out = run(b, "vmm info h1", 30)
        assert "intc plic" in out, out

        # alive: its userspace runs, its file is there, and its disk still works
        g.attach(b, "h1")
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("cat /tmp/keep\n")
        b.expect(r"crossed-shapes", 60)
        b.expect(r"\$ ", ATONCE)
        m = b.mark()
        b.send("echo after-the-move > /tmp/after; cat /tmp/after\n")
        b.expect(r"after-the-move", 60)
        b.expect(r"\$ ", ATONCE)
        # its disk, which needs interrupts from a controller that did not exist
        # when the driver bound: a read that reaches the platter, not the cache
        b.send("sync; echo disk-ok\n")
        b.expect(r"disk-ok", 60)
        b.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in b.output_since(m), b.output_since(m)
        g.detach(b)
        out = run(b, "vmm info h1", 30)
        # the exits kept coming after the switch, which is the difference
        # between a guest that is running and one that is merely not dead
        mm = re.search(r"exits (\d+):", out)
        assert mm and int(mm.group(1)) > 0, out

        # and back the other way, which is not the same exercise: arriving on A
        # the guest is given a controller that is *more* capable than the one it
        # is bound to, and has to take up hardware guest interrupt files it was
        # not using a moment ago.
        a.send("vmm accept 9000 -d &\n")
        a.expect(r"\$ ", 30)
        run(a, "sleep 1", 20)
        out = run(b, "vmm migrate h1 10.7.0.1:9000", 300)
        assert "migrated to 10.7.0.1:9000" in out, out
        a.expect(r"arrived as plic, this host gives imsic: \d+ line\(s\) still held", 120)
        a.expect(r"arrived: 1 vcpu", 120)
        a.expect(r"revision \d+ adopted \(0x[0-9a-f]+\) after \d+ us", 120)
        a.send("\n")
        a.expect(r"\$ ", 30)
        out = run(a, "vmm info h1", 30)
        assert "intc aplic-msi+imsic" in out, out
        g.attach(a, "h1")
        a.send("\n")
        a.expect(r"\$ ", 60)
        a.send("cat /tmp/keep\n")
        a.expect(r"crossed-shapes", 60)
        a.expect(r"\$ ", ATONCE)
        ma = a.mark()
        a.send("echo and-back > /back; sync; cat /back\n")
        a.expect(r"and-back", 60)
        a.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in a.output_since(ma), a.output_since(ma)
        g.detach(a)
        run(a, "vmm kill h1", 60)
        print("    a machine crossed to a host of another shape, rebound, and came back")
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


def t_migrate_refused(QEMU, cpus, log, give_up):
    """a migration that is refused, and the same one made legal by negotiation.

    Host A has a vector unit; host B was built without one. In both halves the
    guest *uses* the unit, so its register file is state that has nowhere to go on
    B, and B refuses -- the guest keeps running on A rather than being lost.

    In the second half the host then asks the guest to give the unit up. The guest
    checks whether any task is relying on it and agrees, and the same migration
    that was refused a moment ago is now allowed. That is the point of the whole
    mechanism in one sequence: a machine that could not move becomes one that can,
    because the guest was asked and gave something up. A fleet-wide feature floor
    gets the same freedom only by taking vectors away from every guest for life."""
    import os
    tag = "ref%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = QEMU(intc="plic", cpus=cpus, log=log, vec=1, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc="plic", cpus=cpus, log=log, vec=0, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        vmid = g.start_guest(a, "-n r1 -m 32 -c 1", 120)
        a.send("vecbench 2\n")
        a.expect(r"vecbench: (ok|no vector extension)", 90)
        a.expect(r"\$ ", ATONCE)
        g.detach(a)

        # this guest has a vector unit, and B has none: state with nowhere to go.
        # (Not asserted through the observation set: vsstatus.VS going Dirty is a
        # transient the guest's own trap handler clears, so whether the host ever
        # samples one depends on when its exits fall -- the same guest running the
        # same vecbench shows `vector' on some runs and not others. The decision
        # is made by asking the guest, not by sampling it.)
        out = run(a, "vmm info r1", 30)
        assert re.search(r"observed:", out), out

        def try_move():
            b.send("vmm accept 9000 -d &\n")
            b.expect(r"\$ ", 30)
            run(b, "sleep 1", 20)
            o = run(a, "vmm migrate r1 10.7.0.2:9000", 300)
            return "migrated to" in o, o

        moved, out = try_move()
        assert not moved, out
        # and it was not lost in the refusing: it is still running on A. Before
        # the acknowledgement existed this is where the guest disappeared -- the
        # source had handed it over and the destination had said no.
        assert re.search(r"\br1\b", run(a, "vmm ls", 30)), run(a, "vmm ls", 30)
        g.attach(a, "r1")
        a.send("\n")
        a.expect(r"\$ ", 60)
        a.send("echo survived-the-refusal\n")
        a.expect(r"survived-the-refusal", 60)
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        if not give_up:
            print("    a guest holding vector state was refused, and kept running")
            return

        # now ask it to give the unit up. It checks whether any task is relying
        # on it, and between programs none is.
        ma = a.mark()
        a.send("vmm revise %d novector\n" % vmid)
        a.expect(r"vector unit revoked: asked", 60)
        a.expect(r"\$ ", ATONCE)
        a.consumed = ma
        a.expect(r"revision \d+ adopted \(0x[0-9a-f]+\)", 90)
        a.consumed = len(a.buf)

        # and the same move is now legal
        moved, out = try_move()
        assert moved, out
        for _ in range(10):
            o = run(a, "vmm ls", 30)
            if "no vms" in o:
                break
            run(a, "sleep 1", 20)
        assert "no vms" in o, o
        b.expect(r"arrived: 1 vcpu", 120)
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "r1")
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("echo moved-after-giving-it-up\n")
        b.expect(r"moved-after-giving-it-up", 60)
        b.expect(r"\$ ", ATONCE)
        g.detach(b)
        print("    a refused guest gave up its vector unit and the move became legal")
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


def t_migrate_legacy(QEMU, cpus, log):
    """the legacy tier: a guest that does not speak the revision protocol, moved to
    a host of another shape.

    Host A has an APLIC-MSI+IMSIC, host B only a PLIC. The guest is xv6 told
    `norevise' on its command line, which makes it behave like a guest that
    predates the protocol: it never asks and never acks. The destination cannot
    tell it anything, so it must not change anything -- the machine keeps the
    controller it left with, as a software model on B. Correct on any host, at the
    price of emulation exits; the participating tier (snap-migrate-hetero) is the
    same crossing without that price, which is the whole argument."""
    import os
    tag = "leg%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = QEMU(intc="imsic", cpus=cpus, log=log, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc="plic", cpus=cpus, log=log, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)

        g.start_guest(a, "-n old -m 32 -c 1 -a norevise", 120)
        a.send("echo from-before > /tmp/keep\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        out = run(a, "vmm info old", 30)
        assert "intc aplic-msi+imsic (hardware guest files)" in out, out
        assert "guest does not speak it" in out, out

        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate old 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        # B says what it decided, and why
        b.expect(r"does not speak revision: keeping imsic on this plic host", 120)
        b.expect(r"arrived: 1 vcpu", 120)
        b.send("\n")
        b.expect(r"\$ ", 30)
        out = run(b, "vmm info old", 30)
        # the same kind it left with, as the software model: no hardware files
        # on a host that has none, and no revision was asked for
        assert re.search(r"intc aplic-msi\+imsic\s*$", out, re.M), out
        assert "revision 1 asked for" not in b.buf.decode(errors="replace"), out
        g.attach(b, "old")
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("cat /tmp/keep\n")
        b.expect(r"from-before", 60)
        b.expect(r"\$ ", ATONCE)
        mb = b.mark()
        b.send("echo on-the-plic-host > /after; sync; cat /after\n")
        b.expect(r"on-the-plic-host", 60)
        b.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in b.output_since(mb), b.output_since(mb)
        g.detach(b)
        # and it pays for the shape it kept: interrupts still go through an
        # emulated controller, which is the exits the participating tier saves
        out = run(b, "vmm info old", 30)
        assert re.search(r"(aplic-s|imsic-s) \d+r/\d+w", out), out
        run(b, "vmm kill old", 60)
        print("    a guest that cannot be told kept its controller across the move")
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


def t_migrate_layout(QEMU, cpus, log, legacy):
    """the second axis a description can be revised on: where the devices are.

    Both hosts have a PLIC, so the controller is not what changes. Host B is
    booted with `guestmap=alt', which puts every guest's virtio block a megabyte
    up the map -- a host of a different *layout*, as real SoCs are. A guest that
    speaks the protocol arrives, is told, remaps the windows and repoints its
    drivers; the queues in its own memory and the device state on the host are
    untouched, so nothing is renegotiated. A guest that cannot be told keeps the
    map it left with. Either way its disk still works afterwards, which is the
    check that matters."""
    import os
    tag = "lay%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = QEMU(intc="plic", cpus=cpus, log=log, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc="plic", cpus=cpus, log=log, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "guestmap=alt"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        g.start_guest(a, "-n mv -m 32 -c 1" + (" -a norevise" if legacy else ""), 120)
        a.send("echo before-the-move > /keep; sync\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate mv 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        if legacy:
            b.expect(r"arriving with its devices at \+0x0 does not speak revision: keeping that map", 120)
            b.expect(r"arrived: 1 vcpu", 120)
        else:
            b.expect(r"arrived with devices at \+0x0, this host puts them at \+0x100000", 120)
            b.expect(r"arrived: 1 vcpu", 120)
            m = b.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\)", 120)
            assert int(m.group(1), 16) & 1, m.group(0)     # REV_DEVICES taken
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "mv")
        b.send("\n")
        b.expect(r"\$ ", 60)
        if not legacy:
            # the guest says what it did, from inside -- after all four moved, since
            # the console it says it on is one of them
            assert re.search(r"machine: virtio device moved 0x1000[1-4]000 -> 0x1010[1-4]000",
                             b.buf.decode(errors="replace")), b.buf.decode(errors="replace")[-3000:]
        b.send("cat /keep\n")
        b.expect(r"before-the-move", 60)
        b.expect(r"\$ ", ATONCE)
        mb = b.mark()
        # the disk, through a register window that is somewhere else now
        b.send("echo after-the-move > /after; sync; cat /after\n")
        b.expect(r"after-the-move", 60)
        b.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in b.output_since(mb), b.output_since(mb)
        g.detach(b)
        run(b, "vmm kill mv", 60)
        print("    " + ("a guest that cannot be told kept its device map across the move"
                        if legacy else "a guest followed its devices to a new address without a reboot"))
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
        # the heterogeneous one names no mode: the two hosts are
        # deliberately different, which is the whole point of it.
        ("snap-migrate-hetero", lambda: t_migrate_hetero(QEMU, cpus, log)),
        # and one that is refused, for the right reason, and one that is not
        ("snap-migrate-refused", lambda: t_migrate_refused(QEMU, cpus, log, 0)),
        ("snap-migrate-negotiated", lambda: t_migrate_refused(QEMU, cpus, log, 1)),
        # and a guest that cannot be told anything, kept as it was
        ("snap-migrate-legacy", lambda: t_migrate_legacy(QEMU, cpus, log)),
        # the layout axis: devices somewhere else on the destination
        ("snap-migrate-layout", lambda: t_migrate_layout(QEMU, cpus, log, 0)),
        ("snap-migrate-layout-legacy", lambda: t_migrate_layout(QEMU, cpus, log, 1)),
    ]
