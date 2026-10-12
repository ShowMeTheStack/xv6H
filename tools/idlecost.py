#!/usr/bin/env python3
# idlecost: what the protocol costs a host when nothing is revised, on one build. The same xv6
# guest (1 vcpu, 32 MB) idles on one IMSIC host, either taking part in the protocol (the default:
# it asks the version at boot, the host keeps its revision state, the guest compares a word at its
# tick) or as a legacy guest (`-a norevise': it never asks, the host treats it as a machine that
# cannot be told). Interleaved A, B, A, B ... N rounds each, a fresh guest each round: boot, settle
# 20 s, then 60 s idle, reading the host's counters for the machine (`vmm info': exits, of which
# MMIO, CSR and WFI sleeps) before and after. The host's own observation hooks run for both arms
# (they are part of the build); what differs is everything the protocol adds for a guest that
# takes part. Prints every round and the medians. The Linux guest's side is kvm/t_idle.py.
#   python3 tools/idlecost.py [rounds]
import os, re, sys, time, statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))   # run as run_script.py at the tree's root
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 10
SETTLE, SECS = 20, 60
LOG = open("console.log", "w")
q = th.QEMU(intc="imsic", cpus=3, log=LOG)

def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)

def counters(name):
    info = host("vmm info %s" % name)
    m = re.search(r"exits (\d+): mmio (\d+), csr (\d+), wfi sleeps (\d+)", info)
    return tuple(int(x) for x in m.groups()) if m else None

res = {"protocol": [], "legacy": []}
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    for r in range(N):
        for arm in ("protocol", "legacy"):
            name = "i%d%s" % (r, arm[0])
            g.start_guest(q, "-n %s -m 32 -c 1%s" % (name, " -a norevise" if arm == "legacy" else ""), 120)
            g.detach(q)
            time.sleep(SETTLE)
            a = counters(name); time.sleep(SECS); b = counters(name)
            if a and b:
                d = [(y - x) / SECS for x, y in zip(a, b)]
                res[arm].append(d)
                print("round %d %-8s: exits %.1f/s (mmio %.1f, csr %.1f, wfi sleeps %.1f)" % (r + 1, arm, *d))
            else:
                print("round %d %-8s: no sample" % (r + 1, arm))
            sys.stdout.flush()
            host("vmm kill %s" % name, 60)
            time.sleep(3)
    for arm, v in res.items():
        if not v:
            continue
        cols = list(zip(*v))
        print("%-8s n=%d exits/s median %.1f [%.1f..%.1f]; mmio %.1f, csr %.1f, wfi sleeps %.1f (medians)" % (
            arm, len(v), statistics.median(cols[0]), min(cols[0]), max(cols[0]),
            statistics.median(cols[1]), statistics.median(cols[2]), statistics.median(cols[3])))
    print("RAW", res)
finally:
    q.close()
