#
# the `ct' suite: a container's network, and what it may and may not
# reach.
#
# The other half of the suite -- namespaces, control groups, the root a
# container pivots into, the syscall filter -- is in test-hyp.py, which
# owns the machines these tests are driven from. What is here is the
# part that needs a gateway, an address and somebody on the other side
# of it: a container is behind its own gateway, it reaches the world
# through one address, and the world reaches it only at the ports that
# were published for it.
#
# The standing assertions of the harness apply to every machine these
# tests start: qemu's user-mode NAT and nothing else, no listening
# socket on the workstation, and nothing on the emulated network able to open
# a connection to it. A container is one more thing behind all of that,
# and the tests below check that it did not become an exception.
#
import re

# the programs a container needs to say anything about its own network
NETROOT = ["sh", "ls", "cat", "echo", "ifconfig", "ping", "netstat", "nc",
           "sleep", "dig"]


def mkroot(q, run, path, progs=NETROOT):
    """a root file system for a container: a few programs and the library.

    (cp does not carry the mode across, so the last two lines are not
    decoration: a program nobody may execute is one the container cannot
    run, and the shell inside would say `not found'.)"""
    run(q, "mkdir -p %s/bin %s/lib" % (path, path), 20)
    for f in progs:
        run(q, "cp /bin/%s %s/bin/%s" % (f, path, f), 30)
    run(q, "cp /lib/libc.so %s/lib/libc.so" % path, 30)
    run(q, "cp /lib/ld.so %s/lib/ld.so" % path, 30)
    run(q, "chmod 755 %s/bin/* %s/lib/*" % (path, path), 30)


def script(q, run, path, lines):
    """a shell script in the container's root, a line at a time: every
    command typed at the console has to fit in its line editor."""
    run(q, "echo '%s' > %s" % (lines[0], path), 20)
    for l in lines[1:]:
        run(q, "echo '%s' >> %s" % (l, path), 20)


def t_ct_net(QEMU, intc, cpus, log, run_cmd):
    """a container's network: its own namespace, its own interface behind a
    gateway, the world through one address -- and the workstation still out of
    reach from inside it."""
    q = QEMU(intc=intc, cpus=cpus, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        mkroot(q, run_cmd, "/ct")

        # with no address it has a network of its own and nothing in it:
        # the loopback works, and there is no route anywhere else. That
        # is the default, because the machine's network is not something
        # a hostile process should be handed.
        script(q, run_cmd, "/ct/alone",
               ["ifconfig", "netstat -r", "ping 127.0.0.1 1", "ping 1.1.1.1 1"])
        out = run_cmd(q, "vmm run -n a1 --rootfs /ct /bin/sh /alone", 120)
        assert "127.0.0.0" in out, out               # its own loopback route
        assert "10.0.2" not in out, out              # and not the machine's
        assert "1 packets transmitted, 1 received" in out, out
        assert "unreachable" in out.lower(), out     # 1.1.1.1: no route

        # behind a gateway it has an address on a private subnet
        out = run_cmd(q, "vmm gw add g1 10.9.0.1/24", 30)
        assert "10.9.0.1/24 inside" in out, out
        script(q, run_cmd, "/ct/net",
               ["ifconfig", "ping 10.9.0.1 2", "ping 1.1.1.1 2",
                "ping 10.0.2.2 1", "ping 10.0.2.15 1",
                "cat /proc/net/dev"])
        out = run_cmd(q, "vmm run -n c1 --rootfs /ct -i 10.9.0.5 /bin/sh /net",
                      180)
        assert "ip 10.9.0.5 mask 255.255.255.0 gateway 10.9.0.1" in out, out
        # the gateway answers for itself -- it is ours, so all of it -- and
        # the world answers through it, for which one reply is the proof and
        # four would only be a statement about the uplink (see ping_counts in
        # test_hyp_net.py).
        import test_hyp_net as netmod
        c = netmod.ping_counts(out, "10.9.0.1")
        assert c == (2, 2), out
        c = netmod.ping_counts(out, "1.1.1.1")
        assert c and c[0] == 2 and c[1] >= 1, out
        # the workstation is not reachable from inside a container, as it is not
        # reachable from the machine or from a guest
        assert out.count("blocked by the network policy") == 1, out
        # the machine itself is, and deliberately: a container's packets
        # leave the gateway onto the same switch a guest's do, and what a
        # guest may reach a container may reach. What is on the far side
        # of the workstation is what neither of them may reach.
        assert "1 packets transmitted, 1 received" in out, out
        # its counters are its own interface's, not the machine's
        assert re.search(r"(?m)^\s*eth0:", out), out
        assert re.search(r"(?m)^\s*lo:", out), out

        # the machine is untouched: its own address, and no route into the
        # container's subnet (the gateway's public address is the only way
        # in, and only at a published port)
        out = run_cmd(q, "ifconfig")
        assert "10.0.2.15" in out, out
        out = run_cmd(q, "netstat -r")
        assert "10.9.0.0" not in out, out
        out = run_cmd(q, "ping 10.9.0.5 1", 30)
        assert "unreachable" in out.lower() or "blocked" in out, out

        # --net host is the old behaviour, and has to be asked for
        script(q, run_cmd, "/ct/host", ["ifconfig"])
        out = run_cmd(q, "vmm run -n h1 --rootfs /ct --net host /bin/sh /host",
                      120)
        assert "10.0.2.15" in out, out
        print("    a container is behind its own gateway, and nowhere near "
              "the workstation")
    finally:
        q.close()


def t_ct_ports(QEMU, intc, cpus, log, run_cmd):
    """published ports: the one door into a container.

    What is published is reachable at the gateway's public address; what
    is not published is not reachable at all, and the difference is
    visible from outside -- a connection nobody let in is never answered,
    where one to a port with nothing behind it is refused at once."""
    q = QEMU(intc=intc, cpus=cpus, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        mkroot(q, run_cmd, "/ct")
        run_cmd(q, "vmm gw add g1 10.9.0.1/24", 30)

        # a container with a service, and one port published for it
        script(q, run_cmd, "/ct/srv", ["nc -l 8080", "nc -l 9090"])
        out = run_cmd(q, "vmm run -d -n s1 --rootfs /ct -i 10.9.0.5 -p 8080 "
                         "/bin/sh /srv", 90)
        assert "container s1" in out, out
        run_cmd(q, "sleep 2", 20)

        # the published one: through the gateway's public address, into
        # the container, and the answer back out
        out = run_cmd(q, "echo ping-in | nc -w 5 10.0.2.100 8080", 60)
        assert "ping-in" in out, out
        out = run_cmd(q, "vmm gw", 30)
        assert re.search(r"dnat [1-9]", out), out

        # the one that is not published: the gateway drops it, so the
        # connection is never answered at all -- a timeout, not a refusal
        out = run_cmd(q, "nc -w 3 10.0.2.100 9090 < /dev/null", 60)
        assert "refused" not in out.lower(), out
        assert "timed out" in out.lower() or "timeout" in out.lower(), out

        # and a port on the gateway with nothing published at all
        out = run_cmd(q, "nc -w 3 10.0.2.100 7777 < /dev/null", 60)
        assert "refused" not in out.lower(), out

        # and from a *virtual machine* on the same switch, which is the
        # other side that matters: a guest is not the host, and what
        # it can reach is what the gateway publishes and nothing else.
        import test_hyp_guest as g
        g.start_guest(q, "-n v1 -m 64 -c 1 -i 10.0.2.50/24", 120)
        out = run_cmd(q, "echo from-a-vm | nc -w 5 10.0.2.100 8080", 60)
        assert "from-a-vm" in out, out
        out = run_cmd(q, "nc -w 3 10.0.2.100 9090 < /dev/null", 60)
        assert "refused" not in out.lower(), out
        assert "timed out" in out.lower() or "timeout" in out.lower(), out
        g.detach(q)
        run_cmd(q, "vmm kill v1", 30)

        run_cmd(q, "vmm stop s1", 30)
        run_cmd(q, "sleep 1", 20)
        # with the container gone, the publication is gone with it
        out = run_cmd(q, "nc -w 3 10.0.2.100 8080 < /dev/null", 60)
        assert "ping-in" not in out, out
        print("    a container is closed but for what was published for it, "
              "from the host and from a machine")
    finally:
        q.close()


def t_ct_isolation(QEMU, intc, cpus, log, run_cmd):
    """two containers behind one gateway can see each other; two behind
    different gateways cannot, even with the same address."""
    q = QEMU(intc=intc, cpus=cpus, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        mkroot(q, run_cmd, "/ct")
        run_cmd(q, "vmm gw add g1 10.9.0.1/24", 30)
        run_cmd(q, "vmm gw add g2 10.9.0.1/24", 30)

        # one listening behind g1, at .5
        script(q, run_cmd, "/ct/l5", ["nc -l 7000"])
        out = run_cmd(q, "vmm run -d -n i1 --rootfs /ct -i 10.9.0.5 "
                         "/bin/sh /l5", 90)
        assert "container i1" in out, out
        run_cmd(q, "sleep 2", 20)

        # another behind the same gateway reaches it
        script(q, run_cmd, "/ct/talk",
               ["ping 10.9.0.5 2", "echo hi | nc -w 4 10.9.0.5 7000"])
        out = run_cmd(q, "vmm run -n i2 --rootfs /ct -i 10.9.0.6 "
                         "/bin/sh /talk", 120)
        assert "2 packets transmitted, 2 received" in out, out

        # behind the other gateway, that address is nobody's: the two
        # gateways own the same subnet and are two segments, so what is
        # behind one is not reachable from behind the other
        script(q, run_cmd, "/ct/probe",
               ["ping 10.9.0.5 2", "nc -w 4 10.9.0.5 7000"])
        out = run_cmd(q, "vmm run -n i4 --rootfs /ct --gw g2 -i 10.9.0.6 "
                         "/bin/sh /probe", 150)
        assert "2 packets transmitted, 0 received" in out, out
        assert "timed out" in out.lower(), out

        # and the same address may exist behind both, which is what makes
        # them two segments rather than one subnet with two doors
        script(q, run_cmd, "/ct/mine", ["ifconfig", "ping 10.9.0.5 1"])
        out = run_cmd(q, "vmm run -n i3 --rootfs /ct --gw g2 -i 10.9.0.5 "
                         "/bin/sh /mine", 120)
        assert "ip 10.9.0.5" in out, out              # taken, and given anyway
        assert "1 packets transmitted, 1 received" in out, out  # itself

        run_cmd(q, "vmm stop i1", 30)
        print("    one gateway is one segment, and two are two")
    finally:
        q.close()


def t_ct_exec(QEMU, intc, cpus, log, run_cmd):
    """a command in a container that is already running, a volume from the
    machine, and a container that really stops when it is stopped."""
    q = QEMU(intc=intc, cpus=cpus, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        mkroot(q, run_cmd, "/ct",
               ["sh", "ls", "cat", "echo", "hostname", "id", "ps", "sleep"])
        run_cmd(q, "mkdir -p /vol", 20)
        run_cmd(q, "echo from-host > /vol/f", 20)
        script(q, run_cmd, "/ct/idle", ["sleep 120"])

        out = run_cmd(q, "vmm run -d -n e1 --rootfs /ct -h boxy "
                         "-v /vol:/data /bin/sh /idle", 90)
        assert "container e1" in out, out
        m = re.search(r"pid (\d+)", out)
        assert m, out
        init = int(m.group(1))
        run_cmd(q, "sleep 2", 20)

        # the volume is there, and it survived the pivot: a mount inside
        # the new root is part of that root
        out = run_cmd(q, "vmm exec e1 /bin/cat /data/f", 90)
        assert "from-host" in out, out
        # the command is inside every one of its namespaces
        out = run_cmd(q, "vmm exec e1 /bin/hostname", 90)
        assert "boxy" in out, out
        out = run_cmd(q, "vmm exec e1 /bin/id -u", 90)
        assert re.search(r"(?m)^0\s*$", out), out
        out = run_cmd(q, "vmm exec e1 /bin/ps", 90)
        assert re.search(r"(?m)^\s+1\s", out), out       # its own numbering
        assert str(init) not in out, out                  # not the machine's
        # and it is as bounded as the container: the filter is on
        out = run_cmd(q, "vmm exec e1 /bin/hostname zz", 90)
        assert "not permitted" in out.lower(), out
        out = run_cmd(q, "hostname")
        assert "xv6h" in out and "zz" not in out, out

        # a container that is stopped is stopped: its init goes, and
        # everything in its namespace goes with it rather than being
        # reparented to the machine's init and left running
        out = run_cmd(q, "pgrep sleep", 30)
        assert re.search(r"(?m)^\d+", out), out
        out = run_cmd(q, "vmm stop e1", 30)
        assert "stopped" in out, out
        run_cmd(q, "sleep 1", 20)
        out = run_cmd(q, "pgrep sleep", 30)
        assert not re.search(r"(?m)^\d+", out), out
        out = run_cmd(q, "vmm ps", 30)
        assert "e1" not in out, out
        # and a name that stands for nothing is said so, not acted on
        out = run_cmd(q, "vmm exec e1 /bin/ls", 30)
        assert "no container" in out, out
        print("    a command inside a running container, and a stop that stops")
    finally:
        q.close()


def t_ct_mount(QEMU, intc, cpus, log, run_cmd):
    """what a container may mount: the file systems that are made
    out of nothing, a second name for a directory it can already reach,
    and nothing that is a disk."""
    q = QEMU(intc=intc, cpus=cpus, log=log)
    try:
        q.expect(r"init: starting sh", 60)
        q.expect(r"\$ ", 10)
        mkroot(q, run_cmd, "/ct",
               ["sh", "ls", "cat", "echo", "mount", "umount", "mkdir"])

        script(q, run_cmd, "/ct/mt", [
            "mkdir -p /m /b /s",
            "mount -t tmpfs none /m",
            "echo in-tmpfs > /m/f",
            "cat /m/f",
            "mount --bind /m /b",
            "cat /b/f",
            "mount -t sysfs none /s",
            "mount -t ext4 /dev/vda /m2",
            "echo 1 > /sys/kernel/tick_hz",
            "umount /m",
            "mount",
        ])
        out = run_cmd(q, "vmm run -n m1 --rootfs /ct /bin/sh /mt", 150)
        assert "none on /m type tmpfs" in out, out      # its own tmpfs
        assert out.count("in-tmpfs") == 2, out          # and a bind of it
        # a disk is the machine's to mount, and so is the machine's own
        # state: /sys is not on the list a container may mount
        assert "/dev/vda on /m2: Operation not permitted" in out, out
        assert "none on /s: Operation not permitted" in out, out

        # and none of it is the machine's: its mounts are its own, and its
        # settings are not the machine's to change from in there
        out = run_cmd(q, "mount")
        assert " /m " not in out and " /b " not in out, out
        out = run_cmd(q, "cat /sys/kernel/tick_hz")
        assert re.search(r"(?m)^\d\d+", out), out        # not 1
        # nor from a container with the machine's own /sys in reach
        out = run_cmd(q, "su user -c 'echo 1 > /sys/kernel/tick_hz'", 30)
        assert "1" not in out.split("\n")[-1], out
        out = run_cmd(q, "cat /sys/kernel/tick_hz")
        assert re.search(r"(?m)^\d\d+", out), out
        print("    a container mounts what is made of nothing, and no disks")
    finally:
        q.close()


def tests(modes, cpus, log, QEMU, run_cmd):
    m = modes[0]
    return [
        ("ct-net", lambda: t_ct_net(QEMU, m, cpus, log, run_cmd)),
        ("ct-ports", lambda: t_ct_ports(QEMU, m, cpus, log, run_cmd)),
        ("ct-isolation", lambda: t_ct_isolation(QEMU, m, cpus, log, run_cmd)),
        ("ct-exec", lambda: t_ct_exec(QEMU, m, cpus, log, run_cmd)),
        ("ct-mount", lambda: t_ct_mount(QEMU, m, cpus, log, run_cmd)),
    ]
