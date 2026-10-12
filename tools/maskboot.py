#!/usr/bin/env python3
# maskboot: is a guest revised down to the masking floor representative of one booted
# there? Two cells, in one run on the same box:
#   booted   a PLIC host with no vector unit whose guests get a 400 kHz timebase
#            (guesttimebase=400000): the Linux guest boots at the floor and is never revised;
#   revised  an IMSIC host with the vector unit and a 10 MHz timebase: the same guest boots
#            native and is revised to the floor (plic, timebase 400000, novector), as in
#            tools/maskfloor.py.
# In each: ROUNDS rounds of maskbench (scalar and vector add, clock_gettime, a page's first
# touch, direct disk reads), and what the guest chose: its clocksource, the timer's rate and
# source as the kernel reports them, the ISA string it runs with, and sched_clock's rate.
#   tools/remote.sh script maskboot tools/maskboot.py [ROUNDS]
import os, re, sys, statistics
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
TO_FLOOR = ["plic", "timebase 400000", "novector"]
CHOSE = ("cat /sys/devices/system/clocksource/clocksource0/current_clocksource; "
         "dmesg | grep -E 'clocksource|riscv-timer|sched_clock|machrev: (timer|interrupt|vector)' | tail -8; "
         "grep -m1 isa /proc/cpuinfo")

def revise(q, vmid, fact):
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
    out = L.linux_cmd(q, CHOSE, 60)
    print("  %s chose:\n    %s" % (name, "\n    ".join(l.strip() for l in out.splitlines() if l.strip())[-1500:]))
    sys.stdout.flush()
    G.detach(q)

data = {}
# booted at the floor
q = G.boot_host(T.QEMU, "plic", 3, open("maskboot-console.log", "w"), vec=0,
                extra_env={"APPEND": "guesttimebase=400000"})
try:
    vmid = L.start_linux(q, "-f /linux.img -c 2 -H -V", timeout=600)
    L.linux_cmd(q, "mount /dev/vda /mnt && cat /mnt/marker", 120)
    G.detach(q)
    cell(q, vmid, "booted", data)
finally:
    q.close()
# revised to the floor
q = G.boot_host(T.QEMU, "imsic", 3, open("maskboot-console2.log", "w"))
try:
    vmid = L.start_linux(q, "-f /linux.img -c 2 -H -V", timeout=300)
    L.linux_cmd(q, "mount /dev/vda /mnt && cat /mnt/marker", 120)
    G.detach(q)
    for f in TO_FLOOR:
        revise(q, vmid, f)
    cell(q, vmid, "revised", data)
finally:
    q.close()

print("\n| measure | unit | booted at the floor | revised to the floor | booted / revised |")
print("|---|---|---|---|---|")
for k in ["vec-scalar", "vec-vector", "clock", "page", "read"]:
    row = []
    for name in ["booted", "revised"]:
        vals = [v for (n, kk, u), vs in data.items() if n == name and kk == k for v in vs if v is not None]
        unit = next((u for (n, kk, u) in data if kk == k and n == name), "")
        row.append((statistics.median(vals), min(vals), max(vals), unit) if vals else None)
    unit = next((r[3] for r in row if r), "")
    cells = ["%.1f (%.1f-%.1f)" % r[:3] if r else "absent" for r in row]
    ratio = "%.2f" % (row[0][0] / row[1][0]) if row[0] and row[1] else "-"
    print("| %s | %s | %s | %s | %s |" % (k, unit, cells[0], cells[1], ratio))
print("RAW", {"%s/%s" % (n, k): v for (n, k, u), v in data.items()})
