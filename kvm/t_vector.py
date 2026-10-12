#!/usr/bin/env python3
# F10 under KVM (KVM patch 3, kvmtool's `vector' fact). The vector unit taken away from a
# running Linux guest and given back, N rounds. Each time: the guest's own view (hwprobe), and a
# vector instruction actually executed -- after `off' it must be illegal in the guest, not merely
# undescribed; after `on' it must compute the right answer again. The disk answers throughout.
import sys, os, re, time
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
h = Host(log=K + "/t_vector.host.log")
ok = bad = 0; lat = {}
try:
    h.run(guest_cmd("g", extra="--revise -n mode=none"))
    wait_log(h, "g", r"linux# ", 300)
    print(guest_do(h, "g", "mount /dev/vda /mnt; vecuser probe; vecuser use 3", 120))
    for i in range(N):
        for val in ("off", "on"):
            before = len(h.run("cat /tmp/g.log", 30))
            print(h.run("lkvm revise -n g vector %s" % val, 120).strip()[-120:])
            m = None
            for _ in range(60):
                m = re.search(r"revise: revision (\d+) adopted \(0x([0-9a-f]+)\) after (\d+) us", h.run("cat /tmp/g.log", 30)[before:])
                if m: break
                time.sleep(1)
            time.sleep(2)
            out = guest_do(h, "g", "vecuser probe; vecuser use 2; echo v-%s-%d > /mnt/v; sync; cat /mnt/v" % (val, i), 120) or ""
            kv = h.run("grep -a -E 'ceded|kept the vector' /tmp/g.log | tail -1", 30)
            if val == "off":
                good = bool(m) and int(m.group(2), 16) & 4 and "no vector unit" in out and "SIGILL" in out
            else:
                good = bool(m) and int(m.group(2), 16) & 4 and "V present" in out and "SIGILL" not in out
            good = good and ("v-%s-%d" % (val, i)) in out
            ok += bool(good); bad += not good
            if m: lat.setdefault(val, []).append(int(m.group(3)) / 1000)
            print("round %d vector %-3s: adopted %s in %s ms; %s; %s" % (i, val, m and m.group(2), m and int(m.group(3)) / 1000,
                  " | ".join(l.strip() for l in out.splitlines() if "vecuser" in l), "OK" if good else "FAILED"))
            print("   kvmtool:", kv.strip().splitlines()[-1:] if kv.strip() else "-")
            sys.stdout.flush()
    for v, l in lat.items():
        l = sorted(l); print("vector %s adoption ms: n=%d min %.1f median %.1f max %.1f" % (v, len(l), l[0], l[len(l)//2], l[-1]))
    print("RESULT ok=%d bad=%d" % (ok, bad))
finally:
    h.close()
