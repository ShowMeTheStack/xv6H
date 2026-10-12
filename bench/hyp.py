#!/usr/bin/env python3
"""
the hypervisor's own numbers, and the system's around them.

    tools/remote.sh script benchhyp bench/hyp.py      (or run it here)

Four things, each measured where it can be measured directly:

  exits     guest/vmbench.c, a bare S-mode payload. What an exit costs can
            only be seen from inside the guest: the host's own clock cannot
            see the entry and the exit around it. Every number is a round
            trip through the hypervisor.
  scheduler user/schedbench.c on the host and in a guest, which says what a
            context switch and a wakeup cost with a hypervisor underneath.
  migration a 48 MB guest dirtying memory as fast as the emulator will go,
            moved to another host: how much crossed while it ran and how
            much with it stopped.
  files     user/fsbench.c on the host's own root (bench/run.py is the one
            that compares it with Linux on the same image).

What it prints is markdown, which is what bench/RESULTS.md is written from.
"""
import os, re, sys, time

HERE = os.path.dirname(os.path.abspath(os.path.dirname(__file__)))
if not os.path.exists(os.path.join(HERE, "test-hyp.py")):
    HERE = os.getcwd()
sys.path.insert(0, HERE)
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", os.path.join(HERE, "test-hyp.py")).load_module()
g = SourceFileLoader("g", os.path.join(HERE, "test_hyp_guest.py")).load_module()


def boot(**kw):
    q = th.QEMU(log=None, **kw)
    q.expect(r"init: starting sh", 120)
    q.expect(r"\$ ", 30)
    return q


def exits():
    """what each kind of exit costs, from inside a guest."""
    q = boot()
    rows = []
    try:
        out = th.run_cmd(q, "vmm start -k /vmbench -m 16 -f none", 180)
        for m in re.finditer(r"vmbench: ([a-z-]+)\s+([\d.]+) ns each", out):
            rows.append((m.group(1), float(m.group(2))))
        q.send("vmm kill --all\n")
        q.expect(r"\$ ", 30)
    finally:
        q.close()
    return rows


def sched():
    """what the scheduler costs on the host, and in a guest."""
    def one(run, where):
        out = run("schedbench -t 2 msg", 180)
        rt = re.search(r"msg roundtrip_ns (\d+)", out)
        out2 = run("schedbench -t 2 wake", 180)
        p50 = re.search(r"wake latency_ns_p50 (\d+)", out2)
        p99 = re.search(r"wake latency_ns_p99 (\d+)", out2)
        return (where, int(rt.group(1)) if rt else 0,
                int(p50.group(1)) if p50 else 0,
                int(p99.group(1)) if p99 else 0)

    rows = []
    q = boot()
    try:
        rows.append(one(lambda c, t: th.run_cmd(q, c, t), "the host"))
        g.start_guest(q, "-n b -m 64 -c 2", 120)
        rows.append(one(lambda c, t: th.run_cmd(q, c, t), "a guest (2 vcpus)"))
        g.detach(q)
        q.send("vmm kill --all\n")
        q.expect(r"\$ ", 30)
    finally:
        q.close()
    return rows


def net():
    """what the network costs: the stack alone, and across a guest's NIC.

    Two numbers. On the host over the loopback
    address there is no device at all and nothing is emulated: that is the
    stack's own cost. Between a guest and its host the same bytes cross the
    software switch and the guest's virtio-net, and the difference is what a
    guest pays for its network.

    The guest is the one that connects, on purpose. A frame
    arriving at a guest is delivered only if it is the guest's DHCP lease, an
    answer to a name it asked about, a reply to something it said, or destined
    for a port published with `vmm port' -- otherwise the bridge drops it and
    says "frame nobody asked for". Measuring in this direction needs no hole
    made in that policy, and the bytes cross exactly the same two devices."""
    rows = []
    q = boot()
    try:
        out = th.run_cmd(q, "netbench -t 2 -s 4096 stream rr", 240)
        rows.append(("the host, to itself (no device)", out))

        # the host listens; a guest on the switch measures against it
        q.send("netbench -l -p 5555 &\n")
        q.expect(r"\$ ", 30)
        q.send("vmm start -n nb -f /disk0.img -i 10.0.2.40/24 -d\n")
        q.expect(r"vm \d+ nb:", 30)
        q.expect(r"\$ ", 15)
        time.sleep(5)
        g.attach(q, "nb")
        q.expect(r"\$ ", 90)
        out = th.run_cmd(q, "netbench -c 10.0.2.15 -p 5555 -t 2 -s 4096 stream rr", 240)
        rows.append(("a guest to its host (switch + virtio-net)", out))
        # g.detach() waits for the host's prompt itself; expecting a second
        # one leaves every command after it a prompt out of step.
        g.detach(q)
        q.send("vmm kill --all\n")
        q.expect(r"\$ ", 30)
    finally:
        q.close()
    parsed = []
    for where, out in rows:
        mb = re.search(r"stream ([\d.]+) MB/s", out)
        us = re.search(r"rr [\d.]+ round trips/s \(([\d.]+) us each\)", out)
        parsed.append((where, float(mb.group(1)) if mb else 0.0,
                       float(us.group(1)) if us else 0.0))
    return parsed


def migration():
    """a busy guest crossing to another host."""
    tag = "bench%d" % os.getpid()
    link = os.path.join(HERE, tag + ".sock")
    try:
        os.unlink(link)
    except OSError:
        pass
    a = th.QEMU(log=None, fsimg=tag + "-a.img",
                extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 120)
        a.expect(r"\$ ", 30)
        b = th.QEMU(log=None, fsimg=tag + "-b.img",
                    extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 120)
        b.expect(r"\$ ", 30)
        th.run_cmd(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
        th.run_cmd(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        b.send("vmm accept 9000 -n m -d &\n")
        b.expect(r"\$ ", 30)
        th.run_cmd(b, "sleep 1", 20)
        g.start_guest(a, "-n m -m 48 -c 1", 120)
        a.send("dd if=/dev/zero of=/tmp/z bs=4096 count=4000 &\n")
        a.expect(r"\$ ", 30)
        g.detach(a)
        t0 = time.time()
        out = th.run_cmd(a, "vmm migrate m 10.7.0.2:9000", 300)
        dt = time.time() - t0
        live = sum(int(x) for x in re.findall(r"pre-copy round \d+: (\d+) KB", out))
        rounds = len(re.findall(r"pre-copy round \d+:", out))
        stop = re.search(r"stopped for (\d+) page\(s\), (\d+) KB", out)
        return {"rounds": rounds, "live_kb": live,
                "stop_pages": int(stop.group(1)) if stop else 0,
                "stop_kb": int(stop.group(2)) if stop else 0,
                "seconds": round(dt, 1)}
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


def files():
    """fsbench on the host's own root."""
    q = boot()
    rows = []
    try:
        out = th.run_cmd(q, "fsbench", 600)
        for m in re.finditer(r"^(\w+)\s+([\d.]+)\s+(\S+)", out, re.M):
            rows.append((m.group(1), m.group(2), m.group(3)))
    finally:
        q.close()
    return rows


def main():
    what = sys.argv[1:] or ["exits", "sched", "migration"]
    if "exits" in what:
        print("\n### What an exit costs\n")
        print("| what the guest did | nanoseconds |")
        print("|---|---|")
        for name, ns in exits():
            print("| `%s` | %.0f |" % (name, ns))
    if "sched" in what:
        print("\n### What a switch and a wakeup cost\n")
        print("| where | pipe round trip (ns) | wakeup p50 (ns) | p99 (ns) |")
        print("|---|---|---|---|")
        for where, rt, p50, p99 in sched():
            print("| %s | %d | %d | %d |" % (where, rt, p50, p99))
    if "migration" in what:
        print("\n### What a migration costs\n")
        r = migration()
        print("A 48 MB guest writing to its tmpfs as fast as the emulator will")
        print("go, moved to another host over a unix-socket wire:\n")
        print("| rounds | sent while it ran | sent with it stopped | pages stopped for | wall clock |")
        print("|---|---|---|---|---|")
        print("| %d | %d KB | %d KB | %d | %.1f s |"
              % (r["rounds"], r["live_kb"], r["stop_kb"], r["stop_pages"], r["seconds"]))
    if "net" in what:
        print("\n### What the network costs\n")
        print("| where | one-way stream | round trip |")
        print("|---|---|---|")
        for where, mb, us in net():
            print("| %s | %.1f MB/s | %.0f us |" % (where, mb, us))
    if "files" in what:
        print("\n### fsbench on the host's own root\n")
        print("| phase | value | unit |")
        print("|---|---|---|")
        for a, b, c in files():
            print("| %s | %s | %s |" % (a, b, c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
