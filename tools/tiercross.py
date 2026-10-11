#!/usr/bin/env python3
# tiercross (R18): the two tiers on the same crossing. An xv6 guest (1 vcpu, 64 MB) on an IMSIC
# host crosses to a PLIC host, either as a legacy guest (`-a norevise': it keeps its controller,
# served by the destination's software IMSIC) or as a participating one revised to the
# destination's native PLIC (the destination booted with guestpolicy=native, so that it revises
# to its own controller rather than the cheapest one). After the arrival the guest runs the same
# workload as revcost.py's write rounds (a 6 MB write and sync), and the destination's counters are
# read around it: controller accesses per interrupt, interrupts, and exits. N rounds per tier,
# interleaved.
#   python3 tools/tiercross.py [rounds]
import os, re, sys, time, statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))   # run as run_script.py at the tree's root
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 4
LOG = open("console.log", "w")
WORK = "rm -rf /bench; fsbench -q -s 6 -n 20; sync"

def run(q, line, t=60):
    m = q.mark(); q.send(line + "\n"); q.expect(r"\$ ", t); return q.output_since(m)

def counters(q, vmid):
    info = run(q, "vmm info %s" % vmid)
    ctl = 0
    for name in ("plic", "aplic-s", "aplic-m", "imsic-s", "imsic-m"):
        m = re.search(r"%s@[0-9a-f]+ (\d+)r/(\d+)w" % re.escape(name), info)
        if m: ctl += int(m.group(1)) + int(m.group(2))
    m = re.search(r"virtio-blk@[0-9a-f]+ (\d+)r/(\d+)w(?:/(\d+)i)?", info)
    irqs = int(m.group(3)) if m and m.group(3) else 0
    m = re.search(r"exits (\d+):", info)
    return ctl, irqs, int(m.group(1)) if m else 0

def crossing(legacy):
    tag = "tc%d%s" % (os.getpid(), "l" if legacy else "p")
    link = os.path.join(th.HERE, tag + ".sock")
    try: os.unlink(link)
    except OSError: pass
    a = th.QEMU(intc="imsic", cpus=3, log=LOG, fsimg=tag + "-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90); a.expect(r"\$ ", 30)
        env = {"LINK": link, "LINKSRV": "off"}
        if not legacy:
            env["APPEND"] = "guestpolicy=native"
        b = th.QEMU(intc="plic", cpus=3, log=LOG, fsimg=tag + "-b.img", extra_env=env)
        b.expect(r"init: starting sh", 90); b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30); run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        assert "2 received" in run(a, "ping 10.7.0.2 2", 60)
        g.start_guest(a, "-n t1 -m 64 -c 1" + (" -a norevise" if legacy else ""), 120)
        a.send(WORK + "\n"); a.expect(r"\$ ", 300)          # warm: the files exist
        g.detach(a)
        b.send("vmm accept 9000 -d &\n"); b.expect(r"\$ ", 30); run(b, "sleep 1", 20)
        out = run(a, "vmm migrate t1 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        m = b.mark(); b.expect(r"arrived: 1 vcpu", 120); time.sleep(3)
        arrival = [l.strip() for l in b.output_since(m).splitlines() if "hyp: vm" in l]
        c0, i0, e0 = counters(b, "t1")
        g.attach(b, "t1"); b.send("\n"); b.expect(r"\$ ", 30)
        b.send(WORK + "\n"); b.expect(r"\$ ", 300)
        g.detach(b)
        c1, i1, e1 = counters(b, "t1")
        di = i1 - i0
        return (c1 - c0) / di if di else float("nan"), di, e1 - e0, arrival
    finally:
        for q in (a, b):
            if q is not None:
                try: q.close()
                except Exception: pass
        for f in (tag + "-a.img", tag + "-b.img", link):
            try: os.unlink(os.path.join(th.HERE, f))
            except OSError: pass

res = {"legacy": [], "participating": []}
for r in range(N):
    for legacy in (True, False):
        k = "legacy" if legacy else "participating"
        ratio, irqs, exits, arrival = crossing(legacy)
        res[k].append((ratio, irqs, exits))
        print("round %d %-13s: %.2f accesses/irq over %d interrupts, exits %d" % (r + 1, k, ratio, irqs, exits))
        for l in arrival[:4]:
            print("   " + l)
        sys.stdout.flush()
for k, v in res.items():
    rr = [x[0] for x in v if x[0] == x[0]]
    print("%-13s n=%d accesses/irq median %.2f [%.2f..%.2f], interrupts median %d, exits median %d [%d..%d]" % (
        k, len(v), statistics.median(rr), min(rr), max(rr), statistics.median(x[1] for x in v),
        statistics.median(x[2] for x in v), min(x[2] for x in v), max(x[2] for x in v)))
