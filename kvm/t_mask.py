#!/usr/bin/env python3
# The masking baseline under KVM. One Linux guest (2 vcpus, its disk) on a KVM host (in-kernel
# AIA, 10 MHz, the V extension): measured at its native description, then revised to the fleet's
# common floor (kvmtool's PLIC, the FPGA's 400 kHz -- a scaled counter, so `time' traps -- and no
# vector unit), measured, revised back, measured again. maskbench in each cell: scalar and vector
# add bandwidth, clock_gettime, a page's first touch, direct disk reads. ROUNDS rounds a cell.
import sys, os, re, time, statistics
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
BENCH = ["maskbench vec 12 10", "maskbench clock 100000", "maskbench page 16", "maskbench read /dev/vda 12"]
TO_FLOOR = ["intc plic", "timebase 400000", "vector off"]
TO_NATIVE = ["intc imsic", "timebase 10000000", "vector on"]
h = Host(log=K + "/t_mask.host.log")
data = {}

def revise(fact):
    before = len(h.run("cat /tmp/g.log", 30))
    h.run("lkvm revise -n g %s" % fact, 120)
    m = None
    for _ in range(90):
        m = re.search(r"revise: revision (\d+) adopted \(0x([0-9a-f]+)\) after (\d+) us", h.run("cat /tmp/g.log", 30)[before:])
        if m: break
        time.sleep(1)
    print("  revise %-18s %s" % (fact, "adopted 0x%s after %.1f ms" % (m.group(2), int(m.group(3)) / 1000) if m else "NOT ADOPTED"))
    sys.stdout.flush()
    time.sleep(2)

def cell(name):
    for r in range(ROUNDS):
        for b in BENCH:
            out = guest_do(h, "g", b, 900) or ""
            for k, v, u in re.findall(r"(?m)^maskbench (\S+) (\S+) (\S+)$", out):
                try:
                    data.setdefault((name, k, u), []).append(float(v))
                except ValueError:
                    data.setdefault((name, k, u), []).append(None)
    print("  %s measured" % name); sys.stdout.flush()

try:
    h.run(guest_cmd("g", extra="--revise -n mode=none"))
    wait_log(h, "g", r"linux# ", 300)
    print(guest_do(h, "g", "mount /dev/vda /mnt; cat /mnt/marker; maskbench vec 2 1", 120))
    cell("native")
    for f in TO_FLOOR:
        revise(f)
    cell("floor")
    for f in TO_NATIVE:
        revise(f)
    cell("native-again")
finally:
    h.close()
print("\n| measure | unit | native | floor | native again | floor / native |")
print("|---|---|---|---|---|---|")
for k in ["vec-scalar", "vec-vector", "clock", "page", "read"]:
    row = []
    for name in ["native", "floor", "native-again"]:
        vals = [v for (n, kk, u), vs in data.items() if n == name and kk == k for v in vs if v is not None]
        unit = next((u for (n, kk, u) in data if kk == k and n == name), "")
        row.append((statistics.median(vals), min(vals), max(vals), unit) if vals else None)
    unit = next((r[3] for r in row if r), "")
    cells = ["%.2f (%.2f-%.2f)" % r[:3] if r else "absent" for r in row]
    ratio = "%.2f" % (row[1][0] / row[0][0]) if row[0] and row[1] else "-"
    print("| %s | %s | %s | %s | %s | %s |" % (k, unit, cells[0], cells[1], cells[2], ratio))
print("RAW", {"%s/%s" % (n, k): v for (n, k, u), v in data.items()})
