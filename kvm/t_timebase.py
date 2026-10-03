#!/usr/bin/env python3
# A.4: F6 under KVM. The guest's timebase revised under a running Linux guest, host's rate
# (QEMU virt: 10 MHz) -> 1 MHz -> 25 MHz -> back, N rounds. After each: the guest adopted the
# timer (0x10) and says what it took; three seconds of `sleep` are three seconds by the
# guest's own clock and three seconds of the host's wall clock (a counter at the wrong rate is
# a factor of ten one way or the other); the disk still answers.
import sys, os, re, time
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2
RATES = [1000000, 25000000, 10000000]
h = Host(log=K + "/t_timebase.host.log")
ok = bad = 0; lat = []
try:
    h.run(guest_cmd("g", extra="--revise"))
    wait_log(h, "g", r"linux# ", 300)
    print(guest_do(h, "g", "mount /dev/vda /mnt && echo gen0 > /mnt/keep && sync && cat /mnt/keep", 120))
    for i in range(N):
        for hz in RATES:
            before = len(h.run("cat /tmp/g.log", 30))
            print(h.run("lkvm revise -n g timebase %d" % hz, 120).strip()[-160:])
            m = None
            for _ in range(60):
                m = re.search(r"revise: revision (\d+) adopted \(0x([0-9a-f]+)\) after (\d+) us", h.run("cat /tmp/g.log", 30)[before:])
                if m: break
                time.sleep(1)
            t0 = time.time()
            out = guest_do(h, "g", "cat /proc/uptime; sleep 3; cat /proc/uptime; cat /mnt/keep; dmesg | grep -E 'machrev: (timer|revision)' | tail -2", 180) or ""
            wall = time.time() - t0
            up = [float(x) for x in re.findall(r"^(\d+\.\d+) ", out, re.M)]
            dg = up[1] - up[0] if len(up) >= 2 else -1
            good = bool(m) and int(m.group(2), 16) & 0x10 and 2.7 < dg < 3.6 and wall < 3 * 3 and "gen0" in out
            ok += bool(good); bad += not good
            if m: lat.append(int(m.group(3)) / 1000)
            print("round %d -> %8d Hz: adopted %s in %s ms; sleep 3 = %.2f s by the guest, %.2f s of host wall (with the harness): %s" % (
                i, hz, m and m.group(2), m and int(m.group(3)) / 1000, dg, wall, "OK" if good else "FAILED\n" + out[-800:]))
            print("   ", " | ".join(l.strip() for l in out.splitlines() if "machrev" in l)[-300:])
            sys.stdout.flush()
    if lat:
        l = sorted(lat); print("timebase adoption ms: n=%d min %.1f median %.1f max %.1f" % (len(l), l[0], l[len(l)//2], l[-1]))
    print("RESULT ok=%d bad=%d" % (ok, bad))
    print(h.run("grep -a -E 'revise: timebase' /tmp/g.log | tail -6", 30))
finally:
    h.close()
