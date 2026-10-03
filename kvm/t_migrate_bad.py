#!/usr/bin/env python3
# M12: the receiver against streams that do not fit. The source's kvmtool damages one record of
# each migration (XV6H_MIG_CORRUPT, migrate.c's test hook); the destination must refuse the
# machine with its reason, the migration must report failure, and the guest must run on at the
# source (it wrote a file before; it reads it and writes another after each refusal). A clean
# migration of the same guest last: the refusals left nothing behind.
#   CASES: "tag:off:val" sets a 32-bit value at byte off of the first record with that tag;
#          "tag:len:n" sends that record n bytes short. Tags: 1 header, 4 vcpu, 9 serial, 10 revise.
import sys, os, re, time, threading
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *
CASES = [("10:32:2", "next_slot 2 (slot_gpa[2] is desc_gpa)", r"revision state .* does not fit"),
         ("4:len:3", "a vcpu record 3 bytes short", r"vcpu record that does not fit"),
         ("1:len:4", "the header 4 bytes short", r"a header of \d+ bytes"),
         ("9:12:-5", "a serial port's txcnt -5", r"serial record that does not fit"),
         ("10:len:100", "the revise record 100 bytes short", r"revision state .* does not fit")]
SOCK = K + "/kvmlink.sock"
try: os.unlink(SOCK)
except OSError: pass
net = lambda server: ["-netdev", "stream,id=n0,server=%s,addr.type=unix,addr.path=%s" % ("on" if server else "off", SOCK),
                      "-device", "virtio-net-device,netdev=n0"]
hosts = {}
def boot(name, server):
    hosts[name] = Host(log=K + "/t_migbad.%s.log" % name, extra=net(server))
ta = threading.Thread(target=boot, args=("A", True)); ta.start(); time.sleep(3)
tb = threading.Thread(target=boot, args=("B", False)); tb.start()
ta.join(); tb.join()
A, B = hosts["A"], hosts["B"]
NONET = "-n mode=none"
ok = 0
try:
    print(A.run("ip addr add 10.77.0.1/24 dev eth0; ip link set eth0 up; ip addr show eth0 | grep inet", 30))
    print(B.run("ip addr add 10.77.0.2/24 dev eth0; ip link set eth0 up; ping -c 2 10.77.0.1 | tail -2", 60))
    # the source's kvmtool reads the hook at each migration: the case is set through a file
    A.run("export XV6H_MIG_CORRUPT_FILE=/tmp/corrupt; " + guest_cmd("g", extra="--revise %s" % NONET))
    wait_log(A, "g", r"linux# ", 300)
    print(guest_do(A, "g", "mount /dev/vda /mnt && echo before > /mnt/keep && sync && cat /mnt/keep", 120))
    for i, (case, what, refusal) in enumerate(CASES + [("", "a clean stream", None)]):
        B.run("pkill -x lkvm; sleep 1; rm -f /tmp/g.log", 30)
        B.run(guest_cmd("g", extra="--revise %s --incoming 7777" % NONET))
        time.sleep(3)
        A.run("echo '%s' > /tmp/corrupt" % case, 30)
        out = A.run("lkvm migrate -n g --to 10.77.0.2:7777", 900)
        time.sleep(3)
        dlog = B.run("grep -a -E 'migrate|revise|Fatal|Error' /tmp/g.log | tail -4", 30)
        slog = A.run("grep -a 'migrate: test' /tmp/g.log | tail -1", 30)
        print("== case %d: %s (%s)\nsource: %s\nhook: %s\ndestination: %s" % (i, what, case or "none", out.strip()[-200:], slog.strip(), dlog.strip()))
        if refusal:
            refused = re.search(refusal, dlog) is not None and "failed: the machine runs on here" in out
            back = guest_do(A, "g", "cat /mnt/keep; echo after-%d > /mnt/f%d; sync; cat /mnt/f%d" % (i, i, i), 120)
            good = refused and bool(back) and "before" in back and ("after-%d" % i) in back
            print("refused with its reason: %s; the guest on the source: %s -> %s" % (refused, (back or "(no answer)").strip().replace("\n", " | "), "ok" if good else "FAILED"))
        else:
            for _ in range(120):
                if "migrate: arrived" in B.run("cat /tmp/g.log", 30): break
                time.sleep(1)
            back = guest_do(B, "g", "cat /mnt/keep; cat /mnt/f0; echo moved > /mnt/fz; sync; cat /mnt/fz", 300)
            good = bool(back) and "before" in back and "after-0" in back and "moved" in back
            print("the clean move: %s -> %s" % ((back or "(no answer)").strip().replace("\n", " | "), "ok" if good else "FAILED"))
        ok += good
        sys.stdout.flush()
    print("RESULT ok=%d of %d" % (ok, len(CASES) + 1))
finally:
    A.close(); B.close()
