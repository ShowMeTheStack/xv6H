#!/usr/bin/env python3
# R17 under KVM: what a guest that speaks the protocol costs its host
# when nothing is revised, against the same guest without it. A: kvmtool with --revise (the anchor
# exists; the module binds and the guest announces itself). B: without (no anchor; the module finds
# nothing). Interleaved A, B, A, B ... N rounds each. Each round: boot, wait for the shell, settle
# 20 s, then 60 s idle: the lkvm process's CPU time (utime + stime, /proc/PID/stat) and, when
# KVM's debugfs is there, the VM's exits.
import sys, os, re, time, statistics
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
N = int(sys.argv[1]) if len(sys.argv) > 1 else 10
SECS = 60
TAG = os.environ.get("IDLETAG", "")
h = Host(log=K + "/t_idle%s.host.log" % TAG)
res = {"A": [], "B": []}

def cpu_and_exits():
    # the lkvm process (all its threads: /proc/PID/stat sums them), and KVM's global exit count.
    # Short commands: the host's console mangles a long line.
    pids = re.findall(r"^(\d+)(?: \d+)*\s*$", h.run("pidof lkvm", 30), re.M)
    if not pids:
        print("  (no lkvm)"); return None
    st = re.search(r"^(\d+) (\d+)\s*$", h.run("cut -d' ' -f14,15 /proc/%s/stat" % pids[-1], 30), re.M)
    ex = re.findall(r"^(\d+)\s*$", h.run("cat /sys/kernel/debug/kvm/exits", 30), re.M)
    if not st:
        print("  (no stat)"); return None
    return (int(st.group(1)) + int(st.group(2))) / HZ, (int(ex[-1]) if ex else None)

try:
    print(h.run("mount -t debugfs none /sys/kernel/debug", 30))
    print(h.run("ls /sys/kernel/debug/kvm | head -5", 30))
    hz = re.findall(r"^(\d+)\s*$", h.run("getconf CLK_TCK", 30), re.M)
    HZ = int(hz[-1]) if hz else 100       # USER_HZ, 100 on Linux (no getconf in this userland)
    for r in range(N):
        for arm in ("A", "B"):
            extra = "--revise -n mode=none" if arm == "A" else ""
            h.run(guest_cmd("g", extra=extra))
            wait_log(h, "g", r"linux# ", 300)
            mod = guest_do(h, "g", "dmesg | grep -c -i machrev", 60) or ""
            time.sleep(20)
            a = cpu_and_exits(); time.sleep(SECS); b = cpu_and_exits()
            if a and b:
                cpu = (b[0] - a[0]) / SECS * 100
                ex = (b[1] - a[1]) / SECS if a[1] is not None and b[1] is not None else None
                res[arm].append((cpu, ex))
                print("round %d %s: lkvm CPU %.2f%% of a core%s; machrev lines in dmesg %s" % (
                    r, arm, cpu, ", exits %.1f/s" % ex if ex is not None else "", mod.strip()[-4:]))
            else:
                print("round %d %s: no sample" % (r, arm))
            sys.stdout.flush()
            h.run("pkill -f 'lkvm run --name g'; sleep 3; pkill -9 -f 'lkvm run --name g'; true", 60)
            time.sleep(3)
    for arm in ("A", "B"):
        c = [x[0] for x in res[arm]]; e = [x[1] for x in res[arm] if x[1] is not None]
        print("%s (%s): n=%d CPU %% median %.2f min %.2f max %.2f%s" % (arm, "protocol" if arm == "A" else "none",
              len(c), statistics.median(c), min(c), max(c),
              "; exits/s median %.1f" % statistics.median(e) if e else ""))
    print("RAW", res)
finally:
    h.close()
