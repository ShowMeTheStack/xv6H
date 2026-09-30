# revcost (R9, re-taken for L13): controller MMIO accesses per interrupt for the same
# guest on a PLIC and on an IMSIC -- the number that pays for the participating tier.
# One IMSIC host; one xv6 guest (1 vcpu, 64 MB) whose controller is exchanged in place
# (no reboot); a 6 MB write-and-sync workload (fsbench) and a read workload per
# controller, N rounds alternating. Accesses = the controller window's reads+writes from
# `vmm info`; interrupts = the rising edges injected on the disk's line ("Ni" in the
# by-device line). Prints per-round accesses/irq and the medians.
#   python3 tools/revcost.py [rounds]
import os, re, sys, statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 12
LOG = open("console.log", "w")
q = th.QEMU(intc="imsic", cpus=3, log=LOG)
def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)
def counters(vmid):
    info = host("vmm info %d" % vmid)
    ctl = 0
    for name in ("plic", "aplic-s", "aplic-m", "imsic-s", "imsic-m"):
        m = re.search(r"%s@[0-9a-f]+ (\d+)r/(\d+)w" % re.escape(name), info)
        if m: ctl += int(m.group(1)) + int(m.group(2))
    m = re.search(r"virtio-blk@[0-9a-f]+ (\d+)r/(\d+)w(?:/(\d+)i)?", info)
    irqs = int(m.group(3)) if m and m.group(3) else 0
    m = re.search(r"exits (\d+):", info)
    return ctl, irqs, int(m.group(1)) if m else 0
def guest(cmd, t=120):
    q.send(cmd + "\n"); q.expect(r"\$ ", t)
res = {"plic": {"write": [], "read": []}, "imsic": {"write": [], "read": []}}
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    vmid = g.start_guest(q, "-c 1 -m 64", 120)
    # fsbench takes -s MB -n FILES now (the first version's "fsbench 6" is a usage error,
    # which "> /dev/null 2>&1" hid: n4b-revcost measured 0 disk interrupts per round)
    guest("rm -rf /bench; fsbench -q -s 6 -n 20; sync")   # warm: the files exist, the cache is primed
    g.detach(q)
    kind = "imsic"
    for r in range(N):
        for work, cmd in (("write", "rm -rf /bench; fsbench -q -s 6 -n 20; sync"), ("read", "cat /bench/big/f* > /dev/null")):
            c0, i0, e0 = counters(vmid)
            g.attach(q, str(vmid)); q.send("\n"); q.expect(r"\$ ", 30)
            guest(cmd)
            g.detach(q)
            c1, i1, e1 = counters(vmid)
            di = i1 - i0
            ratio = (c1 - c0) / di if di else float("nan")
            res[kind][work].append((ratio, di, e1 - e0))
            print("round %d %-5s %-5s: ctl accesses %d over %d interrupts = %.2f/irq, exits %d" % (r, kind, work, c1 - c0, di, ratio, e1 - e0))
        # exchange the controller in place for the next round
        kind = "plic" if kind == "imsic" else "imsic"
        mk = q.mark(); host("vmm revise %d %s" % (vmid, kind), 60)
        q.consumed = mk; q.expect(r"revision \d+ adopted", 60); q.consumed = len(q.buf)
    for k in ("plic", "imsic"):
        for w in ("write", "read"):
            v = [x[0] for x in res[k][w] if x[0] == x[0]]
            irq = [x[1] for x in res[k][w]]; ex = [x[2] for x in res[k][w]]
            if v:
                print("%-5s %-5s: n=%d accesses/irq median %.2f (min %.2f max %.2f), interrupts median %d, exits median %d" % (
                    k, w, len(v), statistics.median(v), min(v), max(v), statistics.median(irq), statistics.median(ex)))
finally:
    q.close()
