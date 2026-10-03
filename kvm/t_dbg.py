#!/usr/bin/env python3
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
h = Host(log=K + "/t_dbg.host.log")
try:
    h.run(guest_cmd("g", extra="--revise"))
    wait_log(h, "g", r"machrev: speaking", 300)
    print(h.run("ls -la /root/.lkvm/ 2>&1; timeout 20 lkvm revise -n g nochange; echo rc=$?", 60))
    time.sleep(3)
    print(h.run("tail -15 /tmp/g.log; ps | grep lkvm | head -5", 30))
finally:
    h.close()
