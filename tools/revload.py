# revload (L16): every fact revised there and back, N times, while the guest is busy --
# fsbench writing and re-reading files and schedbench forking and switching on both
# vcpus -- on one IMSIC host with an xv6 guest (2 vcpus, 64 MB). Pass for a fact: every
# revision adopted, the workloads still running and their own checks passing at the end.
# Reports adoption latency per fact (n = 2N) and the workloads' verdicts.
#   python3 tools/revload.py [rounds]
import os, re, sys, time, statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 5
LOG = open("console.log", "w")
q = th.QEMU(intc="imsic", cpus=3, log=LOG)
def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)
def revise(vmid, words, t=90):
    mk = q.mark(); q.send("vmm revise %d %s\n" % (vmid, words)); q.expect(r"\$ ", 30)
    q.consumed = mk
    mm = q.expect(r"hyp: vm %d: revision \d+ adopted \(0x[0-9a-f]+\) after (\d+) us" % vmid, t)
    q.consumed = len(q.buf)
    return int(mm.group(1)) / 1000.0
FACTS = [  # (name, there, back)
    ("cboz",     "cboz 128",      "cboz 64"),
    ("map",      "map alt",       "map default"),
    ("irqmap",   "irqmap alt",    "irqmap default"),
    ("intcmap",  "intcmap alt",   "intcmap default"),
    ("intc",     "plic",          "imsic"),
    ("hartid",   "hartid 4",      "hartid 0"),
    ("timer",    "timer sbi",     "timer sstc"),
    ("timebase", "timebase 1000000", "timebase 10000000"),
    ("rtc",      "rtc off",       "rtc on"),
    ("cpus",     "cpus 1",        "cpus 2"),
]
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    vmid = g.start_guest(q, "-c 2 -m 64", 120)
    # the load: files written and read back with checks, processes forked and switched, for the whole run
    q.send("fsbench -s 4 -n 64 -q > /tmp/fs.out 2>&1 &\n"); q.expect(r"\$ ", 30)
    q.send("schedbench -t 3000 -c -1 > /tmp/sched.out 2>&1 &\n"); q.expect(r"\$ ", 30)
    time.sleep(2)
    g.detach(q)
    lat = {}
    for name, there, back in FACTS:
        lat[name] = []
        ok = True
        for r in range(N):
            try:
                lat[name].append(revise(vmid, there)); lat[name].append(revise(vmid, back))
            except Exception as e:
                print("%-9s round %d: NOT ADOPTED: %s" % (name, r, str(e)[:100])); ok = False; break
        v = lat[name]
        if v:
            print("%-9s adoption ms n=%d median %.1f p90 %.1f max %.1f%s" % (name, len(v), statistics.median(v), sorted(v)[int(0.9 * (len(v) - 1))], max(v), "" if ok else "  (then a miss)"))
    # the workloads: still alive, and their own checks
    g.attach(q, str(vmid)); q.send("\n"); q.expect(r"\$ ", 60)
    q.send("kill %1 2>/dev/null; kill %2 2>/dev/null; sleep 100; echo LOADS-DONE\n"); q.expect(r"LOADS-DONE", 120); q.expect(r"\$ ", 30)
    m = q.mark(); q.send("cat /tmp/fs.out | tail -3; cat /tmp/sched.out | tail -3; echo _end_\n"); q.expect(r"_end_", 60)
    print("workloads' last words:\n" + q.output_since(m)[-800:])
    g.detach(q)
    print(host("vmm info %d" % vmid)[-500:])
finally:
    q.close()
