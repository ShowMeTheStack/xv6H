#!/usr/bin/env python3
# smptime: linux-disk-smp's two-loops-against-one, timed three ways. In one series of 80 runs
# of that test, every run had two loops in parallel finish in less time than one loop alone (median 0.78),
# with both vCPUs on harts in only 4% of busy ticks -- not parallelism. Either one loop alone
# is slow, or the guest's clock is not wall time. Each round here times one loop and then two
# in parallel by (1) busybox `time' (the guest's clock, as the test does), (2) the guest's
# `date +%s.%N' before and after, and (3) the host side's wall clock around the command.
# If (1) and (2) agree with (3), the guest's clock is wall time and one loop alone is slow.
#   tools/remote.sh script NAME tools/smptime.py [rounds]
import os, re, sys, time, statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))   # run as run_script.py at the tree's root
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
lx = SourceFileLoader("lx", "test_hyp_linux.py").load_module()
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 10
LOOP = "i=0; while [ $i -lt 60000 ]; do i=$((i+1)); done"
LOG = open("console.log", "w")
q = th.QEMU(intc="plic", cpus=3, log=LOG)

def timed(cmd):
    wrap = "d0=$(date +%%s.%%N); time sh -c '%s'; d1=$(date +%%s.%%N); echo DATES $d0 $d1" % cmd
    t0 = time.time()
    out = lx.linux_cmd(q, wrap, 180)
    host = time.time() - t0
    m = re.search(r"real\s+0m\s*([\d.]+)s", out)
    d = re.search(r"DATES ([\d.]+) ([\d.]+)", out)
    return (float(m.group(1)) if m else float("nan"),
            float(d.group(2)) - float(d.group(1)) if d else float("nan"), host)

res = {"one": [], "two": []}
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    lx.start_linux(q, "-f /linux.img -c 2 -H")
    lx.linux_cmd(q, "nproc")
    for r in range(ROUNDS):
        one = timed(LOOP)
        two = timed("(%s) & (%s); wait" % (LOOP, LOOP))
        res["one"].append(one); res["two"].append(two)
        print("round %d: one loop  time %.2f s, date %.2f s, host %.2f s | two loops  time %.2f s, date %.2f s, host %.2f s"
              % ((r,) + one + two))
        sys.stdout.flush()
    g.detach(q)
    for k in ("one", "two"):
        c = list(zip(*res[k]))
        print("%s: median time %.2f, date %.2f, host %.2f s" % (k, statistics.median(c[0]), statistics.median(c[1]), statistics.median(c[2])))
    r = [b[2] / a[2] for a, b in zip(res["one"], res["two"])]
    print("two/one by the host's clock: median %.2f (%.2f-%.2f); by the guest's `time': median %.2f" % (
        statistics.median(r), min(r), max(r), statistics.median([b[0] / a[0] for a, b in zip(res["one"], res["two"])])))
finally:
    q.close()
