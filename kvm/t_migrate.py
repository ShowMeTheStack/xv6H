#!/usr/bin/env python3
# A.6: a running Linux guest moved between two KVM hosts (two QEMU machines joined by a private
# unix-socket link, nothing else on it), by kvmtool's new migration. Then the guest's oracle on
# the destination: the disk it was using (a file written before the move read back after, and
# a new write read back), its clock (uptime continues), and its kernel quiet. With ARRIVE set
# (e.g. "intc plic;timebase 1000000"), the destination asks for those facts on arrival, as a
# host of another shape would, and the guest's adoption of each is checked too.
import sys, os, re, time, threading
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
ARRIVE = os.environ.get("ARRIVE", "")
INTC = os.environ.get("INTC", "")          # plic: both sides boot the guest on kvmtool's PLIC
SOCK = K + "/kvmlink.sock"
try: os.unlink(SOCK)
except OSError: pass
net = lambda server: ["-netdev", "stream,id=n0,server=%s,addr.type=unix,addr.path=%s" % ("on" if server else "off", SOCK),
                      "-device", "virtio-net-device,netdev=n0"]
hosts = {}
def boot(name, server):
    hosts[name] = Host(log=K + "/t_migrate%s.%s.log" % (os.environ.get("TAG", ""), name), extra=net(server))
ta = threading.Thread(target=boot, args=("A", True)); ta.start(); time.sleep(3)
tb = threading.Thread(target=boot, args=("B", False)); tb.start()
ta.join(); tb.join()
A, B = hosts["A"], hosts["B"]
NONET = "-n mode=none"
try:
    print(A.run("ip addr add 10.77.0.1/24 dev eth0; ip link set eth0 up; ip addr show eth0 | grep inet", 30))
    print(B.run("ip addr add 10.77.0.2/24 dev eth0; ip link set eth0 up; ping -c 2 10.77.0.1 | tail -2", 60))
    # the destination waits first; the same machine, plus --incoming
    env = "export XV6H_ARRIVE='%s'; " % ARRIVE if ARRIVE else ""
    ienv = "export XV6H_INTC=%s; " % INTC if INTC else ""
    B.run(ienv + env + guest_cmd("g", extra="--revise %s --incoming 7777" % NONET))
    time.sleep(3)
    print(B.run("grep -a migrate: /tmp/g.log", 30))
    A.run(ienv + guest_cmd("g", extra="--revise %s" % NONET))
    wait_log(A, "g", r"linux# ", 300)
    print(guest_do(A, "g", "mount /dev/vda /mnt && echo before-the-move > /mnt/keep && sync && cat /mnt/keep; cat /proc/uptime", 120))
    t0 = time.time()
    out = A.run("lkvm migrate -n g --to 10.77.0.2:7777", 900)
    print("source:", out.strip()[-400:])
    for _ in range(300):
        log = B.run("cat /tmp/g.log", 30)
        if "migrate: arrived" in log: break
        time.sleep(1)
    print("destination:", "\n".join(l for l in log.splitlines() if "migrate:" in l))
    print("wall time of the move as the harness saw it: %.1f s" % (time.time() - t0))
    time.sleep(3)
    print("destination threads:\n" + B.run("p=$(pidof lkvm | cut -d' ' -f1); for t in /proc/$p/task/*; do echo $(cat $t/comm) $(cat $t/wchan) $(cut -d' ' -f3 $t/stat); done", 60))
    print("source threads (before it exits) are gone; the source's own record:\n" + A.run("grep -a 'migrate: vcpu' /tmp/g.log", 30))
    out = guest_do(B, "g", "cat /mnt/keep; echo after-the-move > /mnt/f2; sync; cat /mnt/f2; cat /proc/uptime; dmesg | tail -5", 300)
    print("guest on the destination:\n" + (out or "(no answer)"))
    good = bool(out) and "before-the-move" in out and "after-the-move" in out
    if ARRIVE:
        time.sleep(5)
        log = B.run("grep -a -E 'on arrival|revise: revision .* adopted|machrev: (revision|interrupt|timer)' /tmp/g.log | tail -12", 30)
        print("arrival:\n" + log)
        out2 = guest_do(B, "g", "cat /mnt/keep; echo after-arrival > /mnt/f3; sync; cat /mnt/f3; cat /proc/uptime", 300)
        print("guest after the arrival revisions:\n" + (out2 or "(no answer)"))
        good = good and bool(out2) and "after-arrival" in out2
    print("RESULT", "ok" if good else "FAILED")
finally:
    A.close(); B.close()
