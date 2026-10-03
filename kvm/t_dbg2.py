#!/usr/bin/env python3
# A.2 debug: one device-map revision, what kvmtool moved and what the module saw
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
h = Host(log=K + "/t_dbg2.host.log")
try:
    h.run(guest_cmd("g", extra="--revise"))
    wait_log(h, "g", r"linux# ", 300)
    print(guest_do(h, "g", "cat /proc/iomem | grep -i virtio; ls /sys/bus/platform/devices | grep -i virtio", 60))
    print(h.run("lkvm revise -n g %s" % (sys.argv[1] if len(sys.argv) > 1 else "devmap alt"), 60))
    time.sleep(20)
    print(guest_do(h, "g", "dmesg | grep machrev | tail; cat /proc/iomem | grep -i virtio; echo hi > /dev/null; ls /mnt", 60))
    print(h.run("grep -a revise: /tmp/g.log | tail -12", 30))
finally:
    h.close()
