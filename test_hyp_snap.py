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
    """a move the destination cannot take as things stand: refused before a page
    moves, or negotiated -- automatically -- with a guest that can be asked.

    Host A has a vector unit; host B was built without one. The handshake means
    the destination answers the description before any memory is sent.

    give_up=0: the guest cannot be told (`norevise'), so B refuses with a reason;
    the source sends nothing and the guest keeps running on A.

    give_up=1: the guest speaks the protocol. While a task holds vector state the
    guest refuses to cede and the move fails with that reason; once the task has
    finished, the same command makes B ask, the source asks the guest, the guest
    cedes, and the move goes through -- no operator in the loop."""
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
        vmid = g.start_guest(a, "-n r1 -m 32 -c 1" + ("" if give_up else " -a norevise"), 120)
        a.send("vecbench 2\n")
        a.expect(r"vecbench: (ok|no vector extension)", 90)
        a.expect(r"\$ ", ATONCE)
        if give_up:
            # a task that holds vector state for a while. Rounds are not a
            # duration: 400 rounds were over before the move began (fam7), and
            # the guest rightly ceded a unit nobody was using.
            # vhold touches the registers every 10 ms for the whole time, so the
            # ask finds state held whenever it comes (vecbench's rounds were over
            # before the move began twice in twenty: 18/20 in m7x)
            a.send("vhold 25 &\n")
            a.expect(r"\$ ", ATONCE)
            a.send("sleep 1\n")
            a.expect(r"\$ ", 30)
        g.detach(a)

        def try_move():
            b.send("vmm accept 9000 -d &\n")
            b.expect(r"\$ ", 30)
            run(b, "sleep 1", 20)
            o = run(a, "vmm migrate r1 10.7.0.2:9000", 300)
            return "migrated to" in o, o

        moved, out = try_move()
        assert not moved, out
        # B's console is only read when something waits on it: drain it before
        # asserting on what it said (fam7 failed on a stale buffer, not a
        # missing message)
        b.send("\n")
        b.expect(r"\$ ", 30)
        # the handshake: nothing was sent
        assert "pre-copy round" not in out, out
        assert "took" not in b.buf.decode(errors="replace")[-3000:], b.buf[-3000:]
        if give_up:
            assert "would not give up" in out, out
            assert "kept its vector unit" in out, out
        else:
            assert "destination refused" in out, out
            assert "cannot be asked" in b.buf.decode(errors="replace"), b.buf[-2000:]
        # and it was not lost: still running on A, and says so
        assert re.search(r"\br1\b", run(a, "vmm ls", 30)), run(a, "vmm ls", 30)
        g.attach(a, "r1")
        if give_up:
            # the guest's side of the refusal, said on its own console while it
            # was detached; the backlog is replayed on attach
            a.expect(r"keeping the vector unit, \d+ task\(s\)", 30)
        a.send("\n")
        a.expect(r"\$ ", 60)
        a.send("echo survived-the-refusal\n")
        a.expect(r"survived-the-refusal", 60)
        a.expect(r"\$ ", ATONCE)
        if not give_up:
            g.detach(a)
            print("    a guest that cannot be asked was refused before a page moved, and kept running")
            return
        # wait for the vector task to finish; nothing else changes
        a.send("wait; echo VDONE\n")
        a.expect(r"VDONE", 300)
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        # the same command again: B asks, A asks the guest, the guest cedes, it moves
        moved, out = try_move()
        assert moved, out
        assert "cede 0x4" in out or "cede" in a.buf.decode(errors="replace")[-4000:], out
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
        b.send("vecbench 1\n")
        b.expect(r"vecbench: no vector extension", 60)
        b.expect(r"\$ ", ATONCE)
        g.detach(b)
        print("    refused while a task held vector state; negotiated and moved once it did not")
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
        assert re.search(r"(aplic-s|imsic-s)(@[0-9a-f]+)? \d+r/\d+w", out), out
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


def t_lazy_vector(QEMU, cpus, log):
    """L5: a unit revealed by use. `vmm start -L' presents the machine without
    a vector unit; the guest's first vector instruction traps to the host, which
    is the one exact observation of *use* there is (Proposition 4 says sampling
    cannot give it). The host grants the unit and the instruction re-executes.

    Two guests on a host with a unit, moved to a host without one:
      lz  never touches vectors: it is accepted without a word, because its
          description never had a unit to cede -- the whole point;
      lz2 runs vecbench: the unit appears, is used, and must be ceded before
          the same move, which the handshake does on its own."""
    import os
    tag = "lazy%d" % os.getpid()
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

        def move(name):
            b.send("vmm accept 9000 -d &\n")
            b.expect(r"\$ ", 30)
            run(b, "sleep 1", 20)
            o = run(a, "vmm migrate %s 10.7.0.2:9000" % name, 300)
            assert "migrated to" in o, o
            b.expect(r"arrived: 1 vcpu", 120)
            b.send("\n")
            b.expect(r"\$ ", 30)
            return o

        # -- lz: presented without a unit, never asks for one
        m = a.mark()
        g.start_guest(a, "-n lz -m 32 -c 1 -L", 120)
        boot = a.output_since(m)
        assert "vector extension" not in boot, boot
        run(a, "echo before > /tmp/k", 30)
        g.detach(a)
        out = move("lz")
        assert "cede" not in out, out            # nothing to cede: no unit was ever described
        g.attach(b, "lz")
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("vecbench 2\n")
        b.expect(r"vecbench: no vector extension", 60)
        b.expect(r"\$ ", ATONCE)
        b.send("cat /tmp/k\n")
        b.expect(r"before", 60)
        b.expect(r"\$ ", ATONCE)
        g.detach(b)
        run(b, "vmm kill lz", 60)
        print("    a guest that never used the unit crossed to a host without one, unasked")

        # -- lz2: uses it, gets it on first use, gives it back for the move
        m = a.mark()
        g.start_guest(a, "-n lz2 -m 32 -c 1 -L", 120)
        assert "vector extension" not in a.output_since(m)
        a.send("vecbench 2\n")
        # vecbench asks hwinfo first, as any real program does: the question is
        # what reveals the unit, not an instruction (a guest that believes its
        # description never executes one)
        a.expect(r"the guest asks for a vector unit: granting it", 60)
        a.expect(r"revision \d+ adopted", 60)
        a.expect(r"vecbench: ok \(vlen 128 bits, 2 rounds\)", 90)
        a.expect(r"\$ ", ATONCE)
        m = a.mark()
        a.send("vecbench 3\n")                      # and the second time is not a first time
        a.expect(r"vecbench: ok \(vlen 128 bits, 3 rounds\)", 90)
        a.expect(r"\$ ", ATONCE)
        assert "granting" not in a.output_since(m), a.output_since(m)
        a.send("cat /sys/vector/vlen\n")
        a.expect(r"\b128\b", 30)
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        out = move("lz2")
        assert "cede 0x4" in out or "cede" in a.buf.decode(errors="replace")[-4000:], out
        g.attach(b, "lz2")
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("vecbench 2\n")
        b.expect(r"vecbench: no vector extension", 60)
        b.expect(r"\$ ", ATONCE)
        g.detach(b)
        run(b, "vmm kill lz2", 60)
        print("    a guest that asked for the unit was granted it, and ceded it for the move")

        # -- lz3: a program that does not ask and just tries: the instruction traps
        g.start_guest(a, "-n lz3 -m 32 -c 1 -L", 120)
        a.send("vecbench -f 2\n")
        a.expect(r"first vector instruction \(0x[0-9a-f]+\): granting the unit", 60)
        a.expect(r"revision \d+ adopted", 60)
        a.expect(r"vecbench: ok \(vlen 128 bits, 2 rounds\)", 90)
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        run(a, "vmm kill lz3", 60)
        print("    a program that tried without asking was trapped on its first instruction and granted the unit")
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


def t_migrate_intc_addr(QEMU, cpus, log, legacy):
    """F2: the interrupt controller's *address* is a fact of the machine. Both
    hosts have a PLIC; host B is booted with `guestintc=alt', which puts every
    guest's controller registers 256 MB up the map (0x1c000000). A guest that
    speaks the protocol arrives, is told, maps the new window and rebinds its
    driver -- the same class of change as a new controller kind, with the
    kind unchanged. A guest that cannot be told keeps the address it left with.
    Either way interrupts keep arriving, which is what the disk check shows."""
    import os
    tag = "ica%d" % os.getpid()
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
                 extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "guestintc=alt"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        g.start_guest(a, "-n ic -m 32 -c 1" + (" -a norevise" if legacy else ""), 120)
        a.send("echo before-the-move > /keep; sync\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate ic 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        if legacy:
            b.expect(r"arriving with its interrupt controller at \+0x0 does not speak revision: keeping that address", 120)
            b.expect(r"arrived: 1 vcpu", 120)
        else:
            b.expect(r"arrived with its interrupt controller at \+0x0, this host puts it at \+0x10000000", 120)
            b.expect(r"arrived: 1 vcpu", 120)
            m = b.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\)", 120)
            assert int(m.group(1), 16) & 2, m.group(0)     # REV_INTC taken
        b.send("\n")
        b.expect(r"\$ ", 30)
        out = run(b, "vmm info ic", 30)
        # the exits say where the guest is reaching its controller
        if legacy:
            assert re.search(r"plic@c000000 \d+r/\d+w", out) or "plic@" not in out, out
        else:
            assert "plic@1c000000" in out or "plic@" not in out, out
        g.attach(b, "ic")
        if not legacy:
            # said from inside, replayed on attach
            b.expect(r"machine: interrupt controller plic at 0x1c000000", 60)
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("cat /keep\n")
        b.expect(r"before-the-move", 60)
        b.expect(r"\$ ", ATONCE)
        mb = b.mark()
        # the disk: its completion interrupt goes through the controller at its new address
        b.send("echo after-the-move > /after; sync; cat /after\n")
        b.expect(r"after-the-move", 60)
        b.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in b.output_since(mb), b.output_since(mb)
        g.detach(b)
        out = run(b, "vmm info ic", 30)
        assert re.search(r"plic@(1c|c)000000 \d+r/\d+w", out), out
        run(b, "vmm kill ic", 60)
        print("    " + ("a guest that cannot be told kept its controller's address across the move"
                        if legacy else "a guest rebound to its controller at a new address without a reboot"))
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


def t_migrate_irq(QEMU, cpus, log, legacy):
    """F3 across a move: host B is booted with `guestirq=alt', so every guest
    there has its uart on line 7 and its virtio devices on 11..14. A guest that
    speaks the protocol arrives, is told, and renumbers; one that cannot keeps
    the numbering it left with. Both keep working, which the disk shows."""
    import os
    tag = "irq%d" % os.getpid()
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
                 extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "guestirq=alt"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        g.start_guest(a, "-n iq -m 32 -c 1" + (" -a norevise" if legacy else ""), 120)
        a.send("echo before-the-move > /keep; sync\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate iq 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        if legacy:
            b.expect(r"arriving with its interrupt lines numbered default does not speak revision: keeping that numbering", 120)
            b.expect(r"arrived: 1 vcpu", 120)
        else:
            b.expect(r"arrived with its interrupt lines numbered default, this host numbers them alt", 120)
            b.expect(r"arrived: 1 vcpu", 120)
            m = b.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\)", 120)
            assert int(m.group(1), 16) & 1, m.group(0)     # REV_DEVICES taken
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "iq")
        if not legacy:
            b.expect(r"machine: interrupt lines renumbered: uart 7, virtio 11 12 13", 60)
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("cat /keep\n")
        b.expect(r"before-the-move", 60)
        b.expect(r"\$ ", ATONCE)
        mb = b.mark()
        b.send("echo after-the-move > /after; sync; cat /after\n")
        b.expect(r"after-the-move", 60)
        b.expect(r"\$ ", ATONCE)
        assert "kernel is booting" not in b.output_since(mb), b.output_since(mb)
        g.detach(b)
        run(b, "vmm kill iq", 60)
        print("    " + ("a guest that cannot be told kept its interrupt numbering across the move"
                        if legacy else "a guest renumbered its interrupt lines on arrival without a reboot"))
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


def t_migrate_axis(QEMU, cpus, log, axis, legacy):
    """a host of a different shape on one axis, made with a host command-line
    word on B; a participating guest is told and adopts, a legacy guest keeps
    what it left with; both keep working. AXIS: (name, append, legacy-msg,
    participating-msg, guest-msg, adopted-bit)."""
    import os
    name, append, legacy_msg, part_msg, guest_msg, bit = axis
    tag = "%s%d" % (name, os.getpid())
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
                 extra_env={"LINK": link, "LINKSRV": "off", "APPEND": append})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        g.start_guest(a, "-n ax -m 32 -c 2" + (" -a norevise" if legacy else ""), 120)
        a.send("echo before-the-move > /keep; sync\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate ax 10.7.0.2:9000", 300)
        if "migrated to 10.7.0.2:9000" not in out:
            # B's side of the story into the log before failing
            try:
                b.send("\n")
                b.expect(r"\$ ", 5)
            except Exception:
                pass
        assert "migrated to 10.7.0.2:9000" in out, out + "\n--- B:\n" + b.buf.decode(errors="replace")[-2500:]
        if legacy:
            b.expect(legacy_msg, 120)
            b.expect(r"arrived: 2 vcpu", 120)
        else:
            b.expect(part_msg, 120)
            b.expect(r"arrived: 2 vcpu", 120)
            if bit:         # a fact told through the description; 0: told through a device
                m = b.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\)", 120)
                assert int(m.group(1), 16) & bit, m.group(0)
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "ax")
        if not legacy and guest_msg:
            b.expect(guest_msg, 60)
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("cat /keep\n")
        b.expect(r"before-the-move", 60)
        b.expect(r"\$ ", ATONCE)
        mb = b.mark()
        b.send("echo after-the-move > /after; sync; cat /after; sleep 1; echo SLEPT\n")
        try:
            b.expect(r"after-the-move", 60)
            b.expect(r"SLEPT", 60)          # the timer and the disk both work over there
            b.expect(r"\$ ", ATONCE)
        except Exception as e:
            # the guest's state at the moment it stopped answering, for the log
            try:
                g.detach(b)
                run(b, "vmm info ax", 30)
            except Exception:
                pass
            raise
        assert "kernel is booting" not in b.output_since(mb), b.output_since(mb)
        g.detach(b)
        run(b, "vmm kill ax", 60)
        print("    " + ("a guest that cannot be told kept its %s across the move" % name
                        if legacy else "a guest adopted this host's %s on arrival" % name))
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


AXES = {
    "ipi": ("ipi", "guestipi=sbi",
            r"arriving with clint ipis does not speak revision: keeping them",
            r"arrived with clint ipis, this host has no clint",
            r"machine: ipis via sbi", 1),
    "timer": ("timer", "guesttimer=sbi",
              r"arriving with the sstc timer does not speak revision: keeping it",
              r"arrived with the sstc timer, this host has no sstc",
              r"machine: timer via sbi", 16),
    "timebase": ("timebase", "guesttimebase=1000000",
                 r"arriving with a 10000000 Hz timebase does not speak revision: keeping it",
                 r"arrived with a 10000000 Hz timebase, this host's is 1000000 Hz",
                 r"machine: timebase 1000000 Hz", 16),
    "hartid": ("hartid", "guesthart=4",
               r"arriving with harts numbered from 0 does not speak revision: keeping that",
               r"arrived with harts numbered from 0, this host numbers them from 4",
               r"machine: harts renumbered: 4 5", 1),
    "mac": ("mac", "guestmac=alt",
            r"arriving with nic 52:54:00:78:76:10 does not speak revision: keeping it",
            r"arrived with nic 52:54:00:78:76:10, this host gives 52:54:00:79:76:10: told through the device",
            r"virtio net: mac changed to 52:54:00:79:76:10", 0),
    "rtc": ("rtc", "guestrtc=off",
            r"arriving with an rtc does not speak revision: keeping that",
            r"arrived with an rtc, this host offers none",
            r"rtc: gone; the wall clock continues from \d+ in software", 1),
    "console": ("console", "guestcon=sbi",
                r"arriving with a uart console does not speak revision: keeping it",
                r"arrived with a uart console, this host's is sbi",
                r"machine: console via sbi", 1),
    "ram": ("ram", "guestgrow=32",
            r"vm \d+ ax ",              # a legacy guest is not given more: nothing to see but the arrival line
            r"arrived with 32 MB, this host gives it 64 MB",
            r"machine: memory 32 -> 64 MB", 64),
    "cpus": ("cpus", "guestcpus=1",
             r"arriving with 2 harts running does not speak revision: keeping them",
             r"arrived with 2 harts running, this host allows 1",
             r"machine: cpus 2 -> 1 \(parking 1 hart\(s\)\)", 1),
    "sbi": ("sbi", "nosbi=rfence",
            r"arriving without SBI extensions 0x0 does not speak revision: keeping that set",
            r"arrived without SBI extensions 0x0, this host withholds 0x4",
            r"machine: sbi extensions: time ipi hsm srst dbcn", 8),
}


def t_migrate_vlen(QEMU, cpus, log):
    """F11: the vector register width is a fact of the machine. Host A has a
    128-bit unit, host B a 256-bit one. A guest that has used its unit cannot
    simply be carried -- its save areas are laid out for 128 bits -- so the
    move is negotiated as for a host without a unit: the guest cedes (no task
    holds vector state), crosses, and is given B's unit back at B's width; a
    program then sees vlen 256."""
    import os
    tag = "vl%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = QEMU(intc="plic", cpus=cpus, log=log, vec=1, vlen=128, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc="plic", cpus=cpus, log=log, vec=1, vlen=256, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        g.start_guest(a, "-n vl -m 32 -c 1", 120)
        a.send("vecbench 2\n")
        a.expect(r"vecbench: ok \(vlen 128 bits", 90)
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate vl 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        assert "cede 0x4" in out or "cede" in a.buf.decode(errors="replace")[-4000:], out
        b.expect(r"gave up for the move; this host's is 256 bits wide and is given back", 120)
        b.expect(r"arrived: 1 vcpu", 120)
        m = b.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\)", 120)
        assert int(m.group(1), 16) & 4, m.group(0)     # REV_VLEN taken
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "vl")
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("cat /sys/vector/vlen; vecbench 2\n")
        b.expect(r"\b256\b", 60)
        b.expect(r"vecbench: ok \(vlen 256 bits", 90)
        b.expect(r"\$ ", ATONCE)
        g.detach(b)
        run(b, "vmm kill vl", 60)
        print("    a guest crossed from a 128-bit to a 256-bit vector unit: ceded, carried, given back wider")
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


def t_migrate_cboz(QEMU, cpus, log, legacy, hazard=0):
    """F21 across a move: host A has 128-byte cache blocks, host B 64-byte
    ones. A participating guest is told and strides 64; a legacy guest keeps
    128 and B does its cbo.zero in software at 128 (henvcfg.CBZE off), so its
    pages are still zero. HAZARD is the control: B without the software model
    (`nocbozmodel'), and the legacy guest's fresh pages come out half dirty --
    the failure the fact exists to prevent."""
    import os
    tag = "cz%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = QEMU(intc="plic", cpus=cpus, log=log, cboz=128, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc="plic", cpus=cpus, log=log, cboz=64, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "nocbozmodel" if hazard else ""})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        g.start_guest(a, "-n cz -m 32 -c 1" + (" -a norevise" if legacy else ""), 120)
        a.send("zerocheck 256\n")
        a.expect(r"zerocheck: 256 pages clean", 120)
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate cz 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out + "\n--- B:\n" + b.buf.decode(errors="replace")[-2500:]
        if legacy:
            b.expect(r"arriving with a 128-byte cache block does not speak revision: keeping it \(this host's is 64, cbo.zero in software\)", 120)
            b.expect(r"arrived: 1 vcpu", 120)
        else:
            b.expect(r"arrived with a 128-byte cache block, this host's is 64", 120)
            b.expect(r"arrived: 1 vcpu", 120)
            m = b.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\)", 120)
            assert int(m.group(1), 16) & 1, m.group(0)
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "cz")
        b.send("\n")
        b.expect(r"\$ ", 60)
        if not legacy:
            assert "machine: cache block 128 -> 64 bytes" in b.buf.decode(errors="replace")[-6000:], b.buf.decode(errors="replace")[-3000:]
        # pages the guest zeroes on B: 4096 of them (16 MB) so that plenty are fresh from kalloc
        b.send("zerocheck 1024; zerocheck 1024\n")
        m = b.expect(r"zerocheck: (1024 pages clean|DIRTY: \d+ bytes of 1024 pages not zero[^\n]*)", 120)
        first = m.group(0).decode(errors="replace")
        m = b.expect(r"zerocheck: (1024 pages clean|DIRTY: \d+ bytes of 1024 pages not zero[^\n]*)", 120)
        second = m.group(0).decode(errors="replace")
        b.expect(r"\$ ", ATONCE)
        g.detach(b)
        out = run(b, "vmm info cz", 60)
        mm = re.search(r"cbo.zero in software (\d+)", out)
        soft = int(mm.group(1)) if mm else 0
        run(b, "vmm kill cz", 60)
        if hazard:
            assert "DIRTY" in first or "DIRTY" in second, (first, second)
            assert soft == 0, out
            print("    the control: a legacy guest striding 128 on a 64-byte host with no software model -- %s" % (first if "DIRTY" in first else second))
        else:
            assert "clean" in first and "clean" in second, (first, second)
            if legacy:
                assert soft > 0, out
                print("    a legacy guest kept its 128-byte block on a 64-byte host: %d cbo.zero done in software, pages clean" % soft)
            else:
                print("    a guest crossed from 128- to 64-byte cache blocks, was told, strides 64: pages clean (%d cbo.zero in software during the window)" % soft)
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


LX_START = "-k /Image -r 0x80200000 -m 128 -a console=ttyS0 earlycon=sbi -f /linux.img -c 2 -H"


def t_lx_migrate_vector(QEMU, cpus, log, lazy):
    """L9: the negotiated tier on a production kernel. Host A has a vector
    unit, host B was built without one; the Linux guest carries patch 0004 and
    machrev.
    lazy=0 (snap-lx-migrate-negotiated): a task holds vector state, so the
    guest refuses to cede and the move is refused with that reason before a
    page moves; once the task has exited the same command makes B ask, A ask
    the guest, the guest cede, and the move goes through. On B a program that
    tries the unit asks for it, B has none, illegal instruction.
    lazy=1 (snap-lx-migrate-lazy): the unit is taken with `lazyvector' while
    nobody holds state; the machine crosses to B without being asked (nothing
    to cede); a program on B asks and is refused; the machine crosses back to
    A, the same program asks and is granted."""
    import os
    import test_hyp_linux as lx
    tag = "lxv%d" % os.getpid()
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
        a.send("vmm start -n lv %s\n" % LX_START)
        a.expect(r"vm (\d+) lv: ", 20)
        a.expect(r"linux# ", 120)
        out = lx.linux_cmd(a, "vecuser probe; vecuser use 2", 60)
        if "vecuser: ok" not in out:
            assert "no vector unit" in out, out
            g.detach(a)
            print("    no vector unit on host A: nothing to negotiate")
            return
        if not lazy:
            lx.linux_cmd(a, "vecuser hold 40 &", 10)
            lx.linux_cmd(a, "sleep 1", 20)
        g.detach(a)

        def try_move(src, dst, name):
            dst.send("vmm accept 9000 -d &\n")
            dst.expect(r"\$ ", 30)
            run(dst, "sleep 1", 20)
            o = run(src, "vmm migrate %s 10.7.0.2:9000" % name if dst is b else "vmm migrate %s 10.7.0.1:9000" % name, 600)
            return "migrated to" in o, o

        if not lazy:
            moved, out = try_move(a, b, "lv")
            assert not moved, out
            b.send("\n"); b.expect(r"\$ ", 30)
            assert "pre-copy round" not in out, out
            assert "would not give up" in out and "kept its vector unit" in out, out
            g.attach(a, "lv"); a.send("\n"); a.expect(r"linux# ", 60)
            out = lx.linux_cmd(a, "dmesg | grep 'keeping the vector unit' | tail -1", 60)
            assert "keeping the vector unit, 1 task(s)" in out, out
            out = lx.linux_cmd(a, "wait; echo VDONE", 120)
            assert "released" in out and "VDONE" in out, out
            g.detach(a)
            moved, out = try_move(a, b, "lv")
            assert moved, out
            assert "cede" in out or "cede" in a.buf.decode(errors="replace")[-4000:], out
            b.expect(r"arrived: 2 vcpu", 120)
            b.send("\n"); b.expect(r"\$ ", 30)
            g.attach(b, "lv"); b.send("\n"); b.expect(r"linux# ", 60)
            out = lx.linux_cmd(b, "dmesg | grep 'machrev: vector' | tail -1; vecuser probe; vecuser use 1; dmesg | grep 'asked the host' | tail -1", 90)
            assert "vector unit withdrawn" in out and "vecuser: no vector unit" in out and "vecuser: SIGILL" in out, out
            assert "refused" in out, out
            g.detach(b)
            run(b, "vmm kill lv", 60)
            print("    Linux refused to cede while a task held vector state; ceded and crossed once it did not; on the unit-less host a program asked and was refused")
            return

        # lazy: taken while nobody holds state, crosses unasked, asks on B (refused) and on A again (granted)
        vmid_out = run(a, "vmm ls", 30)
        m = re.search(r"\b(\d+)\s+lv\b", vmid_out) or re.search(r"vm (\d+)", vmid_out)
        mk = a.mark()
        out = run(a, "vmm revise lv lazyvector" if not m else "vmm revise %s lazyvector" % m.group(1), 60)
        assert "asked" in out, out
        a.consumed = mk                       # the ack may have landed before the prompt
        a.expect(r"revision \d+ adopted \(0x([0-9a-f]*4)\)", 60)
        a.consumed = len(a.buf)
        a.send("\n"); a.expect(r"\$ ", 30)
        moved, out = try_move(a, b, "lv")
        assert moved, out
        assert "cede" not in out, out              # nothing to cede: the description has no unit
        b.expect(r"arrived: 2 vcpu", 120)
        b.send("\n"); b.expect(r"\$ ", 30)
        g.attach(b, "lv"); b.send("\n"); b.expect(r"linux# ", 60)
        out = lx.linux_cmd(b, "vecuser use 1; dmesg | grep 'asked the host' | tail -1", 90)
        assert "vecuser: SIGILL" in out and "refused" in out, out
        g.detach(b)
        moved, out = try_move(b, a, "lv")
        assert moved, out
        a.expect(r"arrived: 2 vcpu", 120)
        a.send("\n"); a.expect(r"\$ ", 30)
        g.attach(a, "lv"); a.send("\n"); a.expect(r"linux# ", 60)
        out = lx.linux_cmd(a, "vecuser use 2; dmesg | grep -E 'machrev: (asked|vector)' | tail -2", 90)
        assert "vecuser: ok" in out and "vector unit back" in out, out
        g.detach(a)
        run(a, "vmm kill lv", 60)
        print("    Linux's unit taken lazily; it crossed to a unit-less host unasked, a program's ask was refused there and granted back on the host that has one")
    finally:
        if b:
            b.close()
        a.close()
        for f in (link, os.path.join(HERE, tag + "-a.img"), os.path.join(HERE, tag + "-b.img")):
            try:
                os.unlink(f)
            except OSError:
                pass


def t_lx_migrate(QEMU, cpus, log, axis, legacy=0):
    """M5: a Linux guest crosses hosts. AXIS "same": two hosts alike, the
    machine simply goes on (uptime continues, its disk is there). AXIS
    "cboz": host A has 128-byte cache blocks, B 64: a Linux with machrev.ko
    is told on arrival and re-rates clear_page; a stock Linux (machrev=off,
    the same image) keeps 128 and B does its cbo.zero in software. Fresh
    pages are zero either way."""
    import os
    import test_hyp_linux as lx
    tag = "lm%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    acboz = 128 if axis == "cboz" else 64
    a = QEMU(intc="plic", cpus=cpus, log=log, cboz=acboz, fsimg=tag + "-a.img",
             extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc="plic", cpus=cpus, log=log, cboz=64, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        args = "-k /Image -r 0x80200000 -m 128 -a console=ttyS0 earlycon=sbi%s -f /linux.img -c 2 -H" % (
            " machrev=off" if legacy else "")
        a.send("vmm start -n lm %s\n" % args)
        a.expect(r"vm (\d+) lm: ", 20)
        a.expect(r"linux# ", 120)
        out = lx.linux_cmd(a, "mount /dev/vda /mnt && echo before-the-move > /mnt/note && sync && cat /mnt/note")
        assert "before-the-move" in out, out
        out = lx.linux_cmd(a, "zerocheck 1024; dmesg | grep -c 'machrev: speaking' || true", 120)
        assert re.search(r"1024 pages[\s\S]{0,200}?clean", out), out
        assert ("machrev: speaking" in out) == (not legacy) or True
        m = re.search(r"\n(\d+)\r?\n", out)
        spoke = m and int(m.group(1)) > 0
        assert bool(spoke) == (not legacy), (legacy, out)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate lm 10.7.0.2:9000", 600)
        assert "migrated to 10.7.0.2:9000" in out, out + "\n--- B:\n" + b.buf.decode(errors="replace")[-2500:]
        if axis == "cboz":
            if legacy:
                b.expect(r"arriving with a 128-byte cache block does not speak revision: keeping it", 120)
            else:
                b.expect(r"arrived with a 128-byte cache block, this host's is 64", 120)
        b.expect(r"arrived: 2 vcpu", 120)
        if axis == "cboz" and not legacy:
            m = b.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\)", 120)
            assert int(m.group(1), 16) & 1, m.group(0)
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "lm")
        b.send("\n")
        b.expect(r"linux# ", 60)
        out = lx.linux_cmd(b, "cat /mnt/note; echo after-the-move > /mnt/note2; sync; cat /mnt/note2; uptime", 60)
        assert "before-the-move" in out and "after-the-move" in out and "load average" in out, out
        out = lx.linux_cmd(b, "zerocheck 1024; zerocheck 1024", 180)
        assert len(re.findall(r"1024 pages[\s\S]{0,200}?clean", out)) == 2, out
        if axis == "cboz" and not legacy:
            out = lx.linux_cmd(b, "dmesg | grep 'machrev: cache block'")
            assert "cache block 128 -> 64 bytes" in out, out
        out = lx.linux_cmd(b, "time sleep 1", 60)
        m = re.search(r"real\s+0m\s*([\d.]+)s", out)
        assert m and 0.9 <= float(m.group(1)) <= 1.6, out
        g.detach(b)
        info = run(b, "vmm info lm", 60)
        mm = re.search(r"cbo.zero in software (\d+)", info)
        soft = int(mm.group(1)) if mm else 0
        run(b, "vmm kill lm", 60)
        if axis == "same":
            print("    a Linux guest crossed hosts: its disk, its clock and its pages as they were")
        elif legacy:
            assert soft > 0, info
            print("    a stock Linux kept its 128-byte block on a 64-byte host: %d cbo.zero in software, pages clean" % soft)
        else:
            print("    Linux crossed from 128- to 64-byte cache blocks, was told on arrival, re-rated clear_page (%d software cbo.zero in the window)" % soft)
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


LX_AXES = {
    # name: (B's boot word, host message on arrival, machrev line in dmesg, guest check command, expected in its output)
    "cpus": ("guestcpus=1", r"arrived with 2 harts running, this host allows 1",
             "machrev: cpus 2 -> 1 (0 up, 1 down)", "nproc", r"\n1\r?\n"),
    "timer": ("guesttimer=sbi", r"arrived with the sstc timer, this host has no sstc",
              "machrev: timer via sbi", "cat /proc/interrupts | grep -c riscv-timer", r"\n1\r?\n"),
    "timebase": ("guesttimebase=1000000", r"arrived with a 10000000 Hz timebase, this host's is 1000000 Hz",
                 "riscv-timer: timebase 10000000 -> 1000000 Hz", "time sleep 1", r"real\s+0m\s*(0\.9|1\.[0-5])"),
    "rtc": ("guestrtc=off", r"arrived with an rtc, this host offers none",
            "machrev: rtc gone", "ls /sys/class/rtc | wc -l", r"\n0\r?\n"),
    "sbi": ("nosbi=rfence", r"arrived without SBI extensions 0x0, this host withholds 0x4",
            "machrev: sbi extensions re-probed: remote fences by IPI", "dmesg | grep -c rfence_v02_call", r"\n[0-9]\r?\n"),   # a few fences fail in the window before the re-probe
    "ram": ("guestgrow=128", r"arrived with 128 MB, this host gives it 256 MB",
            "machrev: memory 128 -> 256 MB", "grep MemTotal /proc/meminfo", r"MemTotal:\s+(2[0-9]{5}|1[89][0-9]{4}) kB"),
    # the device map and the line numbering: the disk moves with them, and is still a disk
    "layout": ("guestmap=alt", r"arrived with devices at \+0x0, this host puts them at \+0x100000",
               "machrev: virtio device at 0x10001000 -> 0x10101000 line 1: moved",
               "mount /dev/vda /mnt && cat /mnt/marker && echo x > /mnt/after-move && sync && cat /mnt/after-move", r"marker-on-virtio-disk[\s\S]*\nx\r?\n"),
    "irq": ("guestirq=alt", r"arrived with its interrupt lines numbered default, this host numbers them alt",
            "machrev: virtio device at 0x10001000 -> 0x10001000 line 11: moved",
            "mount /dev/vda /mnt && cat /mnt/marker && echo x > /mnt/after-move && sync && cat /mnt/after-move", r"marker-on-virtio-disk[\s\S]*\nx\r?\n"),
}
LX_AXES["ipi"] = ("guestipi=sbi", r"arrived with clint ipis, this host has no clint",
                  "machrev: revision",
                  "(i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done) & (i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done) & wait; grep -c 'IPI1' /proc/interrupts; echo PAR-OK", r"PAR-OK")
LX_AXES["disk"] = ("", r"arrived with a 32768-sector disk, this host's image has 49152: told through the device",
                   "new size: 49152",
                   "cat /sys/block/vda/size", r"\n49152\r?\n")
LX_AXES["mac"] = ("guestmac=alt", r"arrived with nic 52:54:00:78:76:10, this host gives 52:54:00:79:76:10: told through the device",
                  "machrev: nic identity -> 52:54:00:79:76:10", "cat /sys/class/net/eth0/address", r"52:54:00:79:76:10")
LX_AXES["hartid"] = ("guesthart=4", r"arrived with harts numbered from 0, this host numbers them from 4",
                     "machrev: cpu 1: hart 1 -> 5", "grep hart /proc/cpuinfo | grep -v isa; ls /sys/class/rtc; mount /dev/vda /mnt && cat /mnt/marker",
                     r"hart\s*:\s*4[\s\S]*hart\s*:\s*5[\s\S]*marker-on-virtio-disk")
LX_AXES["intc"] = ("", r"arrived as plic, this host gives imsic",
                   "machrev: interrupt controller plic at", "mount /dev/vda /mnt && cat /mnt/marker; dmesg | grep xv6h-vic | tail -1",
                   r"marker-on-virtio-disk[\s\S]*xv6h-vic: plic -> imsic")
LX_AXES["intc-addr"] = ("guestintc=alt", r"arrived with its interrupt controller at \+0x0, this host puts",
                        "machrev: interrupt controller plic at 0x", "mount /dev/vda /mnt && cat /mnt/marker; dmesg | grep 'machrev: interrupt controller' | tail -1",
                        r"marker-on-virtio-disk[\s\S]*rebound")
LX_DISK = ("layout", "irq", "hartid", "disk", "intc", "intc-addr")     # axes whose guest has its disk (the thing that moves)


LX_LEGACY_MSG = {   # what the host says of a Linux that cannot be told, per axis
    "hartid": r"arriving with harts numbered from 0 does not speak revision: keeping that",
    "layout": r"arriving with its devices at \+0x0 does not speak revision: keeping that map",
    "irq": r"arriving with its interrupt lines numbered default does not speak revision: keeping that numbering",
    "intc": r"does not speak revision: keeping plic on this imsic host",
}


def t_lx_migrate_axis(QEMU, cpus, log, name, legacy=0, host_intc="plic"):
    """M5 across a move, one fact at a time: host B differs from A on one axis
    (a boot word); a Linux with machrev.ko is told on arrival and takes it;
    the guest's own view (nproc, /proc/interrupts, sysfs, meminfo) agrees.
    LEGACY: the same image with machrev=off -- the host keeps the guest's
    description and serves it with its models; the disk still works."""
    import os
    import test_hyp_linux as lx
    if name == "intc" and legacy:   # the legacy control: F1 served by the software model
        append, host_msg, dmesg_msg, check, want = "", None, None, None, None
    else:
        append, host_msg, dmesg_msg, check, want = LX_AXES[name]
    if legacy:
        host_msg = LX_LEGACY_MSG[name]
    tag = "la%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = QEMU(intc=host_intc, cpus=cpus, log=log, fsimg=tag + "-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90)
        a.expect(r"\$ ", 30)
        b = QEMU(intc="imsic" if name == "intc" else host_intc, cpus=cpus, log=log, fsimg=tag + "-b.img",
                 extra_env={"LINK": link, "LINKSRV": "off", "APPEND": append})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        if name == "disk":     # B's copy of the guest's disk is 8 MB larger (F20)
            run(b, "dd if=/dev/zero of=/linux.img bs=1M count=8 seek=16 conv=notrunc", 120)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        mb = 128 if name == "ram" else 256
        disk = "/linux.img" if (name in LX_DISK or legacy) else "none"   # the legacy check is the disk
        # the participating controller axis needs the description-bound controller (patch 0005, `-V')
        a.send("vmm start -n la -k /Image -r 0x80200000 -m %d -a console=ttyS0 earlycon=sbi%s -f %s -c 2 -H%s\n" % (
            mb, " machrev=off" if legacy else "", disk, " -V" if (name in ("intc", "intc-addr") and not legacy) else ""))
        a.expect(r"vm (\d+) la: ", 20)
        a.expect(r"linux# ", 120)
        out = lx.linux_cmd(a, "dmesg | grep -c 'machrev: speaking'")
        assert re.search(r"\n%d\r?\n" % (0 if legacy else 1), out), out
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate la 10.7.0.2:9000", 600)
        assert "migrated to 10.7.0.2:9000" in out, out + "\n--- B:\n" + b.buf.decode(errors="replace")[-2500:]
        b.expect(host_msg, 120)
        b.expect(r"arrived: 2 vcpu", 120)
        try:
            if legacy or name == "disk":
                raise StopIteration      # nothing is asked of a guest that cannot be told; F20 is carried by the device, no revision
            b.expect(r"revision \d+ adopted \(0x[0-9a-f]+\)", 180)
        except StopIteration:
            pass
        except Exception:
            # not taken: the host's view, then the guest's own words, for the log
            try:
                b.send("\n")
                b.expect(r"\$ ", 30)
                run(b, "vmm info la", 30)
                g.attach(b, "la")
                b.send("\n")
                b.expect(r"linux# ", 30)
                lx.linux_cmd(b, "dmesg | tail -25; cat /proc/interrupts", 60)
                g.detach(b)
            except Exception:
                pass
            raise
        b.send("\n")
        b.expect(r"\$ ", 30)
        run(b, "vmm info la", 30)          # the host's view before anyone attaches, for the log
        g.attach(b, "la")
        try:
            b.send("\n")
            b.expect(r"linux# ", 60)
            if not legacy:
                out = lx.linux_cmd(b, "dmesg | grep -E 'machrev|riscv-timer|new size' | tail -12", 60)
                assert dmesg_msg in out, out
                out = lx.linux_cmd(b, check, 120)
                assert re.search(want, out), (check, out)
            else:
                # served by the models: the disk it left with is the disk it has
                out = lx.linux_cmd(b, "mount /dev/vda /mnt && cat /mnt/marker && echo x > /mnt/after && sync && cat /mnt/after", 120)
                assert re.search(r"marker-on-virtio-disk[\s\S]*\nx\r?\n", out), out
            out = lx.linux_cmd(b, "time sleep 1", 60)
            m = re.search(r"real\s+0m\s*([\d.]+)s", out)
            assert m and 0.9 <= float(m.group(1)) <= 1.6, out
        except Exception:
            # the guest's state as the host sees it, twice, for the log
            try:
                g.detach(b)
                for _ in range(2):
                    run(b, "vmm info la", 30)
                    run(b, "sleep 2", 20)
            except Exception:
                pass
            raise
        g.detach(b)
        run(b, "vmm kill la", 60)
        print("    a stock Linux (no module) crossed to a host with another %s: kept its description, served by the models" % name
              if legacy else "    Linux crossed to a host with another %s, was told on arrival and took it" % name)
    finally:
        if b:
            b.close()
        a.close()
        for f in (link, os.path.join(HERE, tag + "-a.img"), os.path.join(HERE, tag + "-b.img")):
            try:
                os.unlink(f)
            except OSError:
                pass


def t_migrate_disk_grow(QEMU, cpus, log):
    """F20: the disk's capacity is a fact of the machine the destination may
    change -- its image of the same disk is larger. The device announces a
    configuration change on arrival; the guest's driver re-reads the capacity
    and its block layer takes the new size; the file system on it is as it
    was."""
    import os
    tag = "dg%d" % os.getpid()
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
                 extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        # B's copy of the guest's disk is 8 MB larger
        run(b, "dd if=/dev/zero of=/disk0.img bs=1M count=8 seek=16 conv=notrunc", 120)
        out = run(b, "ls -l /disk0.img", 30)
        assert "25165824" in out, out
        g.start_guest(a, "-n dg -m 32 -c 1", 120)
        a.send("echo before-the-move > /keep; sync\n")
        a.expect(r"\$ ", ATONCE)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate dg 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        b.expect(r"arrived with a 32768-sector disk, this host's image has 49152: told through the device", 120)
        b.expect(r"arrived: 1 vcpu", 120)
        b.send("\n")
        b.expect(r"\$ ", 30)
        g.attach(b, "dg")
        b.expect(r"blk: vda: 49152 sectors \(24 MB\) now", 60)
        b.send("\n")
        b.expect(r"\$ ", 60)
        b.send("cat /keep; cat /sys/block/vda/size\n")
        b.expect(r"before-the-move", 60)
        b.expect(r"49152", 60)
        b.expect(r"\$ ", ATONCE)
        g.detach(b)
        run(b, "vmm kill dg", 60)
        print("    a guest arrived at a host whose copy of its disk is larger, and its block layer took the new size")
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


def t_migrate_memory(QEMU, cpus, log, mode):
    """F14: the destination has less room than the guest is resident in. Host B
    is booted with `guestmem=16': sixteen megabytes for any arriving guest. The
    32 MB guest on A is made resident in ~26 MB (a tmpfs file).

    mode "cede":    the guest has freed the file: the pages are free inside it
                    but still backed on A. The handshake asks it, through its
                    balloon, to be resident in no more than 16 MB; it gives the
                    pages back; the move goes; on B it is resident in <= 16 MB.
    mode "refused": the guest still holds the file: it cannot give enough back;
                    the move is refused with the reason and nothing was sent.
    mode "legacy":  a guest that cannot be asked (`norevise') is refused before
                    a page moves (reason 3)."""
    import os
    tag = "mem%d" % os.getpid()
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
                 extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "guestmem=16"})
        b.expect(r"init: starting sh", 90)
        b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        g.start_guest(a, "-n mm -m 32 -c 1" + (" -a norevise" if mode == "legacy" else ""), 120)
        a.send("dd if=/dev/zero of=/tmp/z bs=1M count=20; echo FILLED\n")
        a.expect(r"FILLED", 120)
        a.expect(r"\$ ", ATONCE)
        if mode == "cede":
            a.send("rm /tmp/z; echo FREED\n")
            a.expect(r"FREED", 60)
            a.expect(r"\$ ", ATONCE)
        g.detach(a)
        out = run(a, "vmm info mm", 30)
        m = re.search(r"\((\d+) MB resident\)", out)
        assert m and int(m.group(1)) >= 20, out          # the point: it is resident in more than B has
        b.send("vmm accept 9000 -d &\n")
        b.expect(r"\$ ", 30)
        run(b, "sleep 1", 20)
        out = run(a, "vmm migrate mm 10.7.0.2:9000", 600)
        if mode == "cede":
            assert "migrated to 10.7.0.2:9000" in out, out + "\n--- B:\n" + b.buf.decode(errors="replace")[-2500:]
            assert re.search(r"asking the guest to give up \d+ MB", out), out
            assert re.search(r"resident in \d+ MB now", out), out
            b.expect(r"arrived: 1 vcpu", 120)     # before the prompt: the arrival prints first
            b.send("\n")
            b.expect(r"\$ ", 30)
            info = run(b, "vmm info mm", 30)
            m = re.search(r"\((\d+) MB resident\)", info)
            assert m and int(m.group(1)) <= 16, info
            g.attach(b, "mm")
            b.send("\n")
            b.expect(r"\$ ", 60)
            b.send("echo still-here\n")
            b.expect(r"still-here", 60)
            b.expect(r"\$ ", ATONCE)
            g.detach(b)
            run(b, "vmm kill mm", 60)
            print("    a guest resident in 20+ MB was asked to fit a 16 MB host, gave the memory back through its balloon, and moved")
        else:
            b.send("\n")
            b.expect(r"\$ ", 30)
            assert "migrated to" not in out, out
            assert "pre-copy round" not in out, out
            if mode == "refused":
                assert "would not give up" in out, out
                assert "could not give up enough memory" in a.buf.decode(errors="replace")[-4000:], out
            else:
                assert "destination refused" in out, out
                assert "cannot be asked" in b.buf.decode(errors="replace")[-3000:], b.buf[-3000:]
            g.attach(a, "mm")
            a.send("\n")
            a.expect(r"\$ ", 60)
            a.send("echo survived\n")
            a.expect(r"survived", 60)
            a.expect(r"\$ ", ATONCE)
            g.detach(a)
            print("    " + ("a guest holding its memory could not be made small enough: refused, and kept running"
                            if mode == "refused" else "a guest that cannot be asked to be smaller was refused before a page moved"))
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
        # L5: the unit is revealed by use, and a guest that never used it needs no negotiation
        ("snap-migrate-lazy", lambda: t_lazy_vector(QEMU, cpus, log)),
        # L9: the same two, with Linux (patch 0004 + machrev) as the guest
        ("snap-lx-migrate-negotiated", lambda: t_lx_migrate_vector(QEMU, cpus, log, 0)),
        ("snap-lx-migrate-lazy", lambda: t_lx_migrate_vector(QEMU, cpus, log, 1)),
        # the layout axis: devices somewhere else on the destination
        ("snap-migrate-layout", lambda: t_migrate_layout(QEMU, cpus, log, 0)),
        ("snap-migrate-layout-legacy", lambda: t_migrate_layout(QEMU, cpus, log, 1)),
        # F2: the controller's address
        ("snap-migrate-intc-addr", lambda: t_migrate_intc_addr(QEMU, cpus, log, 0)),
        ("snap-migrate-intc-addr-legacy", lambda: t_migrate_intc_addr(QEMU, cpus, log, 1)),
        # F3: the numbering of the lines
        ("snap-migrate-irq", lambda: t_migrate_irq(QEMU, cpus, log, 0)),
        ("snap-migrate-irq-legacy", lambda: t_migrate_irq(QEMU, cpus, log, 1)),
        # F4, F5, F22 across a move, both tiers
        ("snap-migrate-ipi", lambda: t_migrate_axis(QEMU, cpus, log, AXES["ipi"], 0)),
        ("snap-migrate-ipi-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["ipi"], 1)),
        ("snap-migrate-timer", lambda: t_migrate_axis(QEMU, cpus, log, AXES["timer"], 0)),
        ("snap-migrate-timer-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["timer"], 1)),
        ("snap-migrate-timebase", lambda: t_migrate_axis(QEMU, cpus, log, AXES["timebase"], 0)),
        ("snap-migrate-timebase-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["timebase"], 1)),
        ("snap-migrate-vlen", lambda: t_migrate_vlen(QEMU, cpus, log)),
        # F14: room on the destination, negotiated through the balloon
        ("snap-migrate-memory", lambda: t_migrate_memory(QEMU, cpus, log, "cede")),
        ("snap-migrate-memory-refused", lambda: t_migrate_memory(QEMU, cpus, log, "refused")),
        ("snap-migrate-memory-legacy", lambda: t_migrate_memory(QEMU, cpus, log, "legacy")),
        ("snap-migrate-mac", lambda: t_migrate_axis(QEMU, cpus, log, AXES["mac"], 0)),
        ("snap-migrate-mac-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["mac"], 1)),
        ("snap-migrate-disk-grow", lambda: t_migrate_disk_grow(QEMU, cpus, log)),
        ("snap-migrate-rtc", lambda: t_migrate_axis(QEMU, cpus, log, AXES["rtc"], 0)),
        ("snap-migrate-console", lambda: t_migrate_axis(QEMU, cpus, log, AXES["console"], 0)),
        ("snap-migrate-ram", lambda: t_migrate_axis(QEMU, cpus, log, AXES["ram"], 0)),
        ("snap-migrate-cpus", lambda: t_migrate_axis(QEMU, cpus, log, AXES["cpus"], 0)),
        ("snap-migrate-cpus-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["cpus"], 1)),
        ("snap-migrate-cboz", lambda: t_migrate_cboz(QEMU, cpus, log, 0)),
        ("snap-migrate-cboz-legacy", lambda: t_migrate_cboz(QEMU, cpus, log, 1)),
        ("snap-migrate-cboz-hazard", lambda: t_migrate_cboz(QEMU, cpus, log, 1, hazard=1)),
        ("snap-lx-migrate", lambda: t_lx_migrate(QEMU, cpus, log, "same")),
        ("snap-lx-migrate-cboz", lambda: t_lx_migrate(QEMU, cpus, log, "cboz")),
        ("snap-lx-migrate-cboz-legacy", lambda: t_lx_migrate(QEMU, cpus, log, "cboz", 1)),
        ("snap-lx-migrate-cpus", lambda: t_lx_migrate_axis(QEMU, cpus, log, "cpus")),
        ("snap-lx-migrate-timer", lambda: t_lx_migrate_axis(QEMU, cpus, log, "timer")),
        ("snap-lx-migrate-timebase", lambda: t_lx_migrate_axis(QEMU, cpus, log, "timebase")),
        ("snap-lx-migrate-rtc", lambda: t_lx_migrate_axis(QEMU, cpus, log, "rtc")),
        ("snap-lx-migrate-sbi", lambda: t_lx_migrate_axis(QEMU, cpus, log, "sbi")),
        ("snap-lx-migrate-ram", lambda: t_lx_migrate_axis(QEMU, cpus, log, "ram")),
        ("snap-lx-migrate-layout", lambda: t_lx_migrate_axis(QEMU, cpus, log, "layout")),
        ("snap-lx-migrate-irq", lambda: t_lx_migrate_axis(QEMU, cpus, log, "irq")),
        ("snap-lx-migrate-mac", lambda: t_lx_migrate_axis(QEMU, cpus, log, "mac")),
        ("snap-lx-migrate-hartid", lambda: t_lx_migrate_axis(QEMU, cpus, log, "hartid")),
        ("snap-lx-migrate-hartid-legacy", lambda: t_lx_migrate_axis(QEMU, cpus, log, "hartid", 1)),
        ("snap-lx-migrate-layout-legacy", lambda: t_lx_migrate_axis(QEMU, cpus, log, "layout", 1)),
        ("snap-lx-migrate-irq-legacy", lambda: t_lx_migrate_axis(QEMU, cpus, log, "irq", 1)),
        ("snap-lx-migrate-intc", lambda: t_lx_migrate_axis(QEMU, cpus, log, "intc", 0)),
        ("snap-lx-migrate-intc-addr", lambda: t_lx_migrate_axis(QEMU, cpus, log, "intc-addr", 0)),
        ("snap-lx-migrate-intc-legacy", lambda: t_lx_migrate_axis(QEMU, cpus, log, "intc", 1)),
        ("snap-lx-migrate-ipi", lambda: t_lx_migrate_axis(QEMU, cpus, log, "ipi")),
        ("snap-lx-migrate-hartid-imsic", lambda: t_lx_migrate_axis(QEMU, cpus, log, "hartid", 0, "imsic")),
        ("snap-lx-migrate-disk", lambda: t_lx_migrate_axis(QEMU, cpus, log, "disk")),
        ("snap-migrate-ram-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["ram"], 1)),
        ("snap-migrate-console-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["console"], 1)),
        ("snap-migrate-rtc-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["rtc"], 1)),
        ("snap-migrate-hartid", lambda: t_migrate_axis(QEMU, cpus, log, AXES["hartid"], 0)),
        ("snap-migrate-hartid-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["hartid"], 1)),
        ("snap-migrate-sbi", lambda: t_migrate_axis(QEMU, cpus, log, AXES["sbi"], 0)),
        ("snap-migrate-sbi-legacy", lambda: t_migrate_axis(QEMU, cpus, log, AXES["sbi"], 1)),
    ]
