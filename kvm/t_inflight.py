#!/usr/bin/env python3
# A.3, Theorem 1 under KVM: the controller exchanged (imsic <-> plic) N times while the guest
# keeps its disk busy. A line owed at an exchange that is not raised again on the new
# controller loses a completion: the reader stops, and the kernel says so (a blocked task,
# a virtio timeout). After every exchange: did the reader move on, is the kernel quiet;
# at the end: how many owed lines kvmtool raised again.
import sys, os, re, time
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
N = int(sys.argv[1]) if len(sys.argv) > 1 else 10
h = Host(log=K + "/t_inflight.host.log")
ok = bad = 0; lat = []
try:
    h.run(guest_cmd("g", extra="--revise"))
    wait_log(h, "g", r"linux# ", 300)
    guest_do(h, "g", "echo 5 > /proc/sys/kernel/hung_task_timeout_secs 2>/dev/null; "
                     "(while true; do dd if=/dev/vda of=/dev/null bs=64k count=64 2>/dev/null && echo R >> /tmp/rd; done) & true", 60)    # `& ;` is a syntax error: guest_do appends `; echo`
    time.sleep(5)
    def reads():
        o = guest_do(h, "g", "wc -l < /tmp/rd", 120) or ""
        m = re.search(r"(\d+)", o); return int(m.group(1)) if m else -1
    prev = reads()
    for i in range(N):
        val = ("plic", "imsic")[i % 2]
        before = len(h.run("cat /tmp/g.log", 30))
        h.run("lkvm revise -n g intc %s" % val, 60)
        m = None
        dumped = False
        for w in range(60):
            m = re.search(r"revise: revision (\d+) adopted \(0x([0-9a-f]+)\) after (\d+) us", h.run("cat /tmp/g.log", 30)[before:])
            if m: break
            if w == 6 and not dumped:
                dumped = True
                print("SLOW (6 s, not acknowledged); the vcpus' registers, four dumps a second apart:")
                for k in range(4):
                    d = h.run("lkvm debug -n g -d 2>&1 | head -80", 60)   # the dump comes back to the asker
                    print("--- dump %d\n%s" % (k, d[-5000:]))
                    time.sleep(1)
            time.sleep(1)
        if False and not m:
            # not acknowledged: what is every cpu and every blocked task doing
            mark = len(h.run("cat /tmp/g.log", 30))
            for k in range(4):
                h.run("lkvm debug -n g -d", 30); time.sleep(1)
            print("NOT ADOPTED; the vcpus' registers, four dumps a second apart:")
            regs = h.run("cat /tmp/g.log", 60)[mark:]
            print("\n".join(l for l in regs.splitlines() if re.search(r"(?i)\b(pc|ra|sepc|scause|sstatus|cpu|vcpu)\b", l))[:6000])
        time.sleep(3)
        now = reads()
        k = guest_do(h, "g", "dmesg | grep -c -E 'blocked for more than|virtio.*timeout|hung_task|rcu.*stall'", 60) or ""
        stalls = int((re.findall(r"(\d+)", k) or ["-1"])[-1])
        good = bool(m) and m.group(2) == "2" and now > prev and stalls == 0
        ok += good; bad += not good
        if m: lat.append(int(m.group(3)) / 1000)
        print("exchange %2d -> %-5s adopted %s in %s ms; reads %d -> %d; stalls %d: %s" % (
            i, val, m and m.group(2), m and int(m.group(3)) / 1000, prev, now, stalls, "OK" if good else "FAILED"))
        sys.stdout.flush()
        prev = now
    log = h.run("grep -a 'raised again' /tmp/g.log", 30)
    lv = sum(int(x) for x in re.findall(r"(\d+) held level", log)); vs = sum(int(x) for x in re.findall(r"(\d+) virtio status", log))
    print("owed and raised again over %d exchanges: %d held level(s), %d virtio status(es)" % (N, lv, vs))
    if lat:
        l = sorted(lat); print("intc adoption ms under load: n=%d min %.1f median %.1f max %.1f" % (len(l), l[0], l[len(l)//2], l[-1]))
    print("RESULT ok=%d bad=%d" % (ok, bad))
finally:
    h.close()
