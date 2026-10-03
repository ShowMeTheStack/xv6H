#!/usr/bin/env python3
# harness check: how much of a typed line reaches the guest's shell, whole and in chunks
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
h = Host(log=K + "/t_con.host.log")
try:
    h.run(guest_cmd("g", extra="--revise"))
    wait_log(h, "g", r"linux# ", 300)
    s = "".join("%03d-" % i for i in range(40))           # 160 bytes, every offset visible
    h.run("echo 'echo W%s' > /tmp/g.in" % s, 30); time.sleep(15)
    print("WHOLE:", h.run("tail -c 600 /tmp/g.log", 30)[-500:])
    for i in range(0, len(s), 24):
        h.run("echo -n 'C%s' > /tmp/g.in" % s[i:i+24], 30); time.sleep(1)
    h.run("echo '' > /tmp/g.in", 30); time.sleep(10)
    print("CHUNKED:", h.run("tail -c 600 /tmp/g.log", 30)[-500:])
    print(h.run("grep -c . /tmp/g.log; ls -la /tmp/g.in; cat /proc/$(pidof lkvm | cut -d\" \" -f1)/status | grep -E \"State|Threads\"", 30))
finally:
    h.close()
