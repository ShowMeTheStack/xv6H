#!/usr/bin/env python3
# A.1 test: the protocol round trip under KVM with the unchanged guest (no fact changes).
import sys, re, time
sys.path.insert(0, __import__("os").path.dirname(__file__))
from kvmh import *
N = int(sys.argv[1]) if len(sys.argv) > 1 else 5
h = Host(log=K + "/t_proto.host.log")
try:
    h.run(guest_cmd("g", extra="--revise"))
    out = wait_log(h, "g", r"linux# |machrev: speaking", 300)
    print("guest up:", bool(out))
    print("\n".join(l for l in (out or "").splitlines() if "machrev" in l or "revise" in l)[:2000])
    for i in range(N):
        r = h.run("lkvm revise -n g nochange", 60)
        print("revise ->", r.strip().splitlines()[-1] if r.strip() else "(nothing)")
        time.sleep(3)
    out = h.run("cat /tmp/g.log", 30)
    print("\n".join(l.strip() for l in out.splitlines() if "revis" in l or "machrev" in l or "stale" in l)[-3000:])
finally:
    h.close()
