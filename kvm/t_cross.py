#!/usr/bin/env python3
# a running Linux guest crossing back and forth between two KVM hosts, revised on
# every arrival, as a host of another shape would. Two KVM hosts (QEMU, joined by a private
# unix-socket link) are booted once; the guest is moved A -> B -> A ... N times by kvmtool's
# stop-and-copy migration (`lkvm migrate`), and the destination asks, on arrival, for its own
# facts:
#   intc      A -> B "intc plic",        B -> A "intc imsic"          (F1)
#   timebase  A -> B "timebase 1000000", B -> A "timebase 10000000"   (F6)
#   both      both facts in one arrival revision
#   alt       intc and timebase on alternate round trips (the endurance run)
# Every arrival is checked: the revision adopted (its time from kvmtool's ask to the ack), the
# file written on the first host read back, a new file written and read back, and the guest's
# uptime gone on from the source's (its clock did not go back). The run stops at the first
# problem, keeping both hosts' logs.
#   python3 -u t_cross.py MODE N        (on the build box, in ~/xv6h-kvm)
import sys, os, re, time, threading, statistics
sys.path.insert(0, os.path.dirname(__file__))
from kvmh import *

MODE = sys.argv[1] if len(sys.argv) > 1 else "alt"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 20
TAG = os.environ.get("TAG", MODE)
FACTS = {
    "intc": ("intc plic", "intc imsic"),
    "timebase": ("timebase 1000000", "timebase 10000000"),
    "both": ("intc plic;timebase 1000000", "intc imsic;timebase 10000000"),
}
def arrive(i):
    """the facts the destination of crossing i (0-based) asks for: even i lands on B"""
    m = MODE if MODE != "alt" else ("intc" if (i // 2) % 2 == 0 else "timebase")
    return FACTS[m][i % 2]

SOCK = K + "/kvmlink.sock"
try: os.unlink(SOCK)
except OSError: pass
net = lambda server: ["-netdev", "stream,id=n0,server=%s,addr.type=unix,addr.path=%s" % ("on" if server else "off", SOCK),
                      "-device", "virtio-net-device,netdev=n0"]
IP = {"A": "10.77.0.1", "B": "10.77.0.2"}
hosts = {}
def boot(name, server):
    hosts[name] = Host(log=K + "/t_cross.%s.%s.log" % (TAG, name), extra=net(server))
ta = threading.Thread(target=boot, args=("A", True)); ta.start(); time.sleep(3)
tb = threading.Thread(target=boot, args=("B", False)); tb.start()
ta.join(); tb.join()
H = hosts
NONET = "-n mode=none"
ADOPT = re.compile(r"revise: revision (\d+) adopted \(0x([0-9a-f]+)\) after (\d+) us")
adopt_ms = []; by_mask = {}; pause_ms = []; ok = 0; problem = None

def uptime(out):
    m = re.findall(r"^(\d+\.\d+) \d+\.\d+\s*$", out or "", re.M)
    return float(m[-1]) if m else None

try:
    print(H["A"].run("ip addr add %s/24 dev eth0; ip link set eth0 up; ip addr show eth0 | grep inet" % IP["A"], 30))
    print(H["B"].run("ip addr add %s/24 dev eth0; ip link set eth0 up; ping -c 2 %s | tail -2" % (IP["B"], IP["A"]), 60))
    H["A"].run(guest_cmd("g", extra="--revise %s" % NONET))
    wait_log(H["A"], "g", r"linux# ", 300)
    out = guest_do(H["A"], "g", "mount /dev/vda /mnt && echo first-host > /mnt/keep && sync && cat /mnt/keep; cat /proc/uptime", 120)
    print(out); last_up = uptime(out)
    src, dst = "A", "B"
    for i in range(N):
        facts = arrive(i)
        H[dst].run("export XV6H_ARRIVE='%s'; " % facts + guest_cmd("g", extra="--revise %s --incoming 7777" % NONET))
        time.sleep(3)
        t0 = time.time()
        out = H[src].run("lkvm migrate -n g --to %s:7777" % IP[dst], 900)
        log = ""
        for _ in range(300):
            log = H[dst].run("cat /tmp/g.log", 30)
            if "migrate: arrived" in log: break
            time.sleep(1)
        wall = time.time() - t0
        if "migrate: arrived" not in log:
            problem = "crossing %d: the guest did not arrive on %s; source said: %s" % (i + 1, dst, out.strip()[-300:]); break
        m = re.search(r"paused for (\d+) ms", out)
        if m: pause_ms.append(float(m.group(1)))
        # the arrival asks one revision per fact; wait for every acknowledgement (120 s at most).
        # The destination's log is this arrival's alone (a fresh kvmtool), so every line counts.
        want = len(facts.split(";")); got = []
        for _ in range(120):
            got = ADOPT.findall(H[dst].run("grep -a -E 'revise: revision .* adopted' /tmp/g.log", 30))
            if len(got) >= want: break
            time.sleep(1)
        if len(got) < want:
            problem = "crossing %d (%s -> %s, %s): %d of %d revisions adopted within 120 s\n%s" % (
                i + 1, src, dst, facts, len(got), want, H[dst].run("tail -40 /tmp/g.log", 30)); break
        for g in got:
            adopt_ms.append(int(g[2]) / 1000.0); by_mask.setdefault(g[1], []).append(int(g[2]) / 1000.0)
        out = guest_do(H[dst], "g", "cat /mnt/keep; echo crossing-%d > /mnt/f; sync; cat /mnt/f; cat /proc/uptime" % (i + 1), 300)
        up = uptime(out)
        good = bool(out) and "first-host" in out and ("crossing-%d" % (i + 1)) in out and up is not None and (last_up is None or up >= last_up)
        if not good:
            problem = "crossing %d (%s -> %s, %s): the oracle failed (uptime %s after %s)\n%s" % (
                i + 1, src, dst, facts, up, last_up, out); break
        last_up = up; ok += 1
        print("  crossing %3d %s->%s %-30s adopted %s, wall %5.1f s, uptime %.1f" % (
            i + 1, src, dst, facts, " ".join("0x%s in %.1f ms" % (g[1], int(g[2]) / 1000.0) for g in got), wall, up)); sys.stdout.flush()
        src, dst = dst, src
    print("CROSSINGS OK: %d of %d" % (ok, N))
    if adopt_ms:
        s = sorted(adopt_ms)
        print("adoption ms: n=%d min=%.1f median=%.1f p90=%.1f max=%.1f" % (
            len(s), s[0], statistics.median(s), s[min(len(s) - 1, int(0.9 * len(s)))], s[-1]))
    for mk, v in sorted(by_mask.items()):
        v = sorted(v)
        print("adoption ms, revision 0x%s: n=%d min=%.1f median=%.1f p90=%.1f max=%.1f" % (
            mk, len(v), v[0], statistics.median(v), v[min(len(v) - 1, int(0.9 * len(v)))], v[-1]))
    if pause_ms:
        print("paused ms: n=%d median=%.0f max=%.0f" % (len(pause_ms), statistics.median(pause_ms), max(pause_ms)))
    print("PROBLEMS: %d" % (1 if problem else 0))
    if problem:
        print(problem)
finally:
    for h in H.values():
        h.close()
