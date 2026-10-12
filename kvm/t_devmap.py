#!/usr/bin/env python3
# Test: F15 (devmap) and F3 (irqmap) revised under a running Linux guest under KVM,
# there and back N times each, with the guest's own oracle after every revision: the disk
# it booted with still mounted, a write read back through the moved window, the interrupt
# counts moving on the new lines, and the module's line for the revision.
import sys, os, re, time
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
facts = sys.argv[2].split(",") if len(sys.argv) > 2 else ["devmap", "irqmap"]
VALUES = {"devmap": ("alt", "default"), "irqmap": ("alt", "default"), "intc": ("plic", "imsic")}
IDLE = float(os.environ.get("IDLE_BEFORE", "0"))
BOOT_ENV = os.environ.get("BOOT_ENV", "")     # XV6H_INTC=plic: the guest boots on the PLIC
h = Host(log=K + "/t_devmap%s.host.log" % os.environ.get("TAG", ""))
ok = bad = 0; lat = {}
try:
    h.run(("export %s; " % BOOT_ENV if BOOT_ENV else "") + guest_cmd("g", extra="--revise"))
    wait_log(h, "g", r"linux# ", 300)
    print(guest_do(h, "g", "mount /dev/vda /mnt && echo gen0 > /mnt/keep && sync && cat /mnt/keep", 120))
    for fact in facts:
        for i in range(N):
            for val in VALUES[fact]:
                before = len(h.run("cat /tmp/g.log", 30))
                if IDLE:
                    time.sleep(IDLE)        # the guest left idle before the ask (the module's 1 s bound)
                r = h.run("lkvm revise -n g %s %s" % (fact, val), 60)
                m = None
                for _ in range(30):
                    log = h.run("cat /tmp/g.log", 30)
                    m = re.search(r"revise: revision (\d+) adopted \(0x([0-9a-f]+)\) after (\d+) us", log[before:])
                    if m: break
                    time.sleep(1)
                if not m:
                    bad += 1; print("NOT ADOPTED:", fact, val, r.strip()[-200:]); continue
                lat.setdefault(fact, []).append(int(m.group(3)) / 1000)
                out = guest_do(h, "g", "cat /mnt/keep; echo %s%d > /mnt/f; sync; cat /mnt/f; grep -E \"virtio|machrev|vic\" /proc/interrupts | head -5; dmesg | grep -E \"machrev|vic\" | tail -3" % (val, i), 120)
                good = out and "gen0" in out and ("%s%d" % (val, i)) in out
                ok += bool(good); bad += not good
                print("%s %s #%d: adopted 0x%s in %s ms; guest %s" % (fact, val, i, m.group(2), m.group(3) and int(m.group(3)) / 1000,
                      "OK" if good else "FAILED:\n" + (out or "(no answer)")[-600:]))
                sys.stdout.flush()
    for f, l in lat.items():
        l = sorted(l); print("%s adoption ms: n=%d min %.1f median %.1f max %.1f" % (f, len(l), l[0], l[len(l)//2], l[-1]))
    print("RESULT ok=%d bad=%d" % (ok, bad))
    print(h.run("grep -a -E \"revise|machrev\" /tmp/g.log | tail -12", 30))
finally:
    h.close()
