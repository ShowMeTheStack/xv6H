#!/usr/bin/env python3
# The masking baseline, on xv6H/QEMU.
#
# Masking gives every guest the fleet's common floor, so it can go anywhere without being told
# anything; revision gives it the host's own description and tells it when that changes. The
# floor is taken from the fleet this work has: QEMU virt (10 MHz, APLIC+IMSIC, vector, Zicboz)
# and the ZedBoard's Rocket (400 kHz, PLIC, no vector, no Zicboz). Masking cannot change a
# counter's rate, so on the QEMU host the floor's 400 kHz is a scaled counter, read by trap.
#
# One Linux guest (2 vcpus, the description-bound controller, its disk) on a QEMU host:
#   native  -> revised to the floor (plic, timebase 400000, novector) -> floor
#           -> revised back (imsic, timebase 10000000, vector)       -> native again
# (Zicboz stays: its presence is not a revisable fact, only its block size is)
# In each cell, ROUNDS rounds of maskbench: vector and scalar add bandwidth, clock_gettime, a
# page's first touch, direct disk reads. Medians and the spread are printed; nothing is
# dropped.
#   tools/remote.sh script mask tools/maskfloor.py [ROUNDS]
import os, re, sys, time, statistics
HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(HERE) == "tools":
    HERE = os.path.dirname(HERE)
os.chdir(HERE)
sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("testhyp", os.path.join(HERE, "test-hyp.py"))
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
import test_hyp_guest as G, test_hyp_linux as L

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
BENCH = ["maskbench vec 12 10", "maskbench clock 100000", "maskbench page 16", "maskbench read /dev/vda 12"]
# Zicboz's presence is not revisable (F21 revises the block size): a floor without it is a boot
# fact, so both cells keep the host's Zicboz and the comparison says so
TO_FLOOR = ["plic", "timebase 400000", "novector"]
TO_NATIVE = ["imsic", "timebase 10000000", "vector"]

def revise(q, vmid, fact):
    mv = q.mark()
    q.send("vmm revise %d %s\n" % (vmid, fact))
    m = q.expect(r"revision (\d+) adopted \(0x([0-9a-f]+)\) after (\d+) us", 120)
    q.send("\n"); q.expect(r"\$ ", 60)
    g = lambda i: m.group(i).decode() if isinstance(m.group(i), bytes) else m.group(i)
    print("  revise %-18s adopted 0x%s after %s us" % (fact, g(2), g(3))); sys.stdout.flush()

def cell(q, vmid, name, data):
    G.attach(q, str(vmid)); q.send("\n"); q.expect(r"linux# ", 60)
    for r in range(ROUNDS):
        for b in BENCH:
            out = L.linux_cmd(q, b, 600)
            for k, v, u in re.findall(r"(?m)^maskbench (\S+) (\S+) (\S+)\r?$", out):
                try:
                    data.setdefault((name, k, u), []).append(float(v))
                except ValueError:
                    data.setdefault((name, k, u), []).append(None)
    out = L.linux_cmd(q, "dmesg | grep -E 'machrev: (timer|interrupt|vector|cache)' | tail -4", 60)
    print("  %s: %s" % (name, " | ".join(l.strip() for l in out.splitlines() if "machrev" in l)[-400:]))
    G.detach(q)

data = {}
q = G.boot_host(T.QEMU, "imsic", 3, open("mask-console.log", "w"))
try:
    vmid = L.start_linux(q, "-f /linux.img -c 2 -H -V", timeout=300)
    out = L.linux_cmd(q, "mount /dev/vda /mnt && cat /mnt/marker", 120)
    G.detach(q)
    cell(q, vmid, "native", data)
    for f in TO_FLOOR:
        revise(q, vmid, f)
    cell(q, vmid, "floor", data)
    for f in TO_NATIVE:
        revise(q, vmid, f)
    cell(q, vmid, "native-again", data)
finally:
    q.close()

print("\n| measure | unit | native | floor | native again | floor / native |")
print("|---|---|---|---|---|---|")
for k in ["vec-scalar", "vec-vector", "clock", "page", "read"]:
    row = []
    for name in ["native", "floor", "native-again"]:
        vals = [v for (n, kk, u), vs in data.items() if n == name and kk == k for v in vs if v is not None]
        unit = next((u for (n, kk, u) in data if kk == k and n == name), "")
        row.append((statistics.median(vals), min(vals), max(vals), unit) if vals else None)
    unit = next((r[3] for r in row if r), "")
    cells = ["%.1f (%.1f-%.1f)" % r[:3] if r else "absent" for r in row]
    ratio = "%.2f" % (row[1][0] / row[0][0]) if row[0] and row[1] else "-"
    print("| %s | %s | %s | %s | %s | %s |" % (k, unit, cells[0], cells[1], cells[2], ratio))
print("RAW", {"%s/%s" % (n, k): v for (n, k, u), v in data.items()})
