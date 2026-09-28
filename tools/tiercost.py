# tiercost: what each tier costs, in exits. One IMSIC host, one xv6 guest
# (2 vcpu, 64 MB). For each description the guest can be given -- the
# native one, then each software model in turn (SBI timer, scaled
# timebase, software cbo.zero, PLIC model on the IMSIC host) -- the host's
# exit counters (`vmm info`) are read around (a) ten seconds of idle and
# (b) a fixed disk workload (stressfs) and (c) a fixed fork/exec workload
# (usertests -q's short set is too long; schedbench is the guest's own).
#   python3 tools/tiercost.py [repeats]
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
R = int(sys.argv[1]) if len(sys.argv) > 1 else 3
TIERS = [   # name, words to get there, words to come back
    ("native (sstc, 10 MHz, cboz 64, imsic file)", None, None),
    ("timer: SBI model", "timer sbi", "timer sstc"),
    ("timebase: scaled 1 MHz", "timebase 1000000", "timebase 10000000"),
    ("cboz: software 128", "cboz 128", "cboz 64"),
    ("intc: PLIC model", "plic", "imsic"),
]
WORK = [("idle 10 s", "sleep 10", 60), ("fsbench 2 MB", "rm -rf /bench; fsbench -q -s 2 -n 20", 600), ("schedbench", "schedbench", 300)]
LOG = open("console.log", "w")
q = th.QEMU(intc="imsic", cpus=3, log=LOG)
def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)
def exits(vmid):
    out = host("vmm info %d" % vmid, 30)
    m = re.search(r"exits (\d+): mmio (\d+), csr (\d+), wfi sleeps (\d+), injected (\d+)(, cbo.zero in software (\d+))?", out)
    assert m, out
    return [int(m.group(i) or 0) for i in (1, 2, 3, 4, 5, 7)]
def revise(vmid, words):
    q.send("vmm revise %d %s\n" % (vmid, words))
    # the adopted line and the next prompt come in either order (adoptprobe)
    q.expect(r"revision \d+ adopted \(0x[0-9a-f]+\) after (\d+) us", 60)
    # resync on something unique: a prompt may be left over from `vmm revise'
    q.send("echo _rs_\n"); q.expect(r"\n_rs_\r?\n", 60); q.expect(r"\$ ", 60)
    host("sleep 5", 20)      # let the guest settle on the new description
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    vmid = g.start_guest(q, "-c 2 -m 64")
    g.detach(q)
    print("%-42s %-10s %4s %10s %10s %10s %10s %8s %8s" % ("tier", "work", "n", "exits", "mmio", "csr", "wfi", "inj", "cbozsw"))
    for name, there, back in TIERS:
        if there:
            revise(vmid, there)
        for wname, cmd, t in WORK:
            tot = None
            for r in range(R):
                e0 = exits(vmid)
                t0 = time.time()
                g.attach(q, vmid)
                q.send("\n"); q.expect(r"\$ ", 30)
                # a marker, as the Linux tests do: a prompt is not unique
                q.send(cmd + "; echo _tc_done_\n"); q.expect(r"\n_tc_done_\r?\n", t)
                dt = time.time() - t0
                g.detach(q)
                e1 = exits(vmid)
                d = [b - a for a, b in zip(e0, e1)] + [dt]
                tot = d if tot is None else [x + y for x, y in zip(tot, d)]
            m = [x / R for x in tot]
            print("%-42s %-10s %4d %10.0f %10.0f %10.0f %10.0f %8.0f %8.0f   %.1f s" % ((name, wname, R) + tuple(m)))
            sys.stdout.flush()
        if back:
            revise(vmid, back)
finally:
    q.close()
