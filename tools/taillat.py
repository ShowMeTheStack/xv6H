#!/usr/bin/env python3
# taillat: what an application sees while its machine is revised (xv6H/QEMU).
#
# A Linux guest (2 vcpus, the description-bound controller, its disk, an address on the host's
# switch) runs `kvlat serve'; the xv6H host runs `kvlat load' against it, open loop, RATE requests
# a second for SECS seconds in 100 ms bins, and a revision is asked 5 s in. For each event:
# the baseline (the bins before the event), the worst bin after it (p99, max), errors, and the time
# until the p99 is back within twice the baseline's. ROUNDS events per fact.
#   tools/remote.sh script tail tools/taillat.py [ROUNDS] [RATE]
import os, re, sys, time, statistics
HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(HERE) == "tools":
    HERE = os.path.dirname(HERE)
os.chdir(HERE)
sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("testhyp", os.path.join(HERE, "test-hyp.py"))
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
import test_hyp_guest as G, test_hyp_linux as L

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 3
RATE = int(sys.argv[2]) if len(sys.argv) > 2 else 500
SECS = 12
EVENTS_ALL = [("none", [None]), ("F1 controller", ["plic", "imsic"]), ("F15 device map", ["map alt", "map default"]),
          ("F6 timebase", ["timebase 1000000", "timebase 10000000"]), ("F9 hart numbering", ["hartid 4", "hartid 0"]),
          ("F7/F8 hart count", ["cpus 1", "cpus 2"])]
ONLY = os.environ.get("ONLY") or (sys.argv[3] if len(sys.argv) > 3 else None)   # e.g. F9: those events alone
if ONLY == "all":                                                                 # (an empty argument does not survive the shell)
    ONLY = None
EVENTS = [e for e in EVENTS_ALL if not ONLY or e[0].startswith(ONLY) or e[0] == "none"]

def alive(q, why):
    # after every event: the guest answers on its console, and the host reaches it
    m = q.mark()
    C.send("ping %s 1\n" % TARGET); C.expect(r"\$ ", 60)
    G.attach(q, str(vmid)); q.send("\n")
    try:
        q.expect(r"linux# ", 60)
        out = L.linux_cmd(q, "cat /proc/interrupts | grep -iE 'virtio|eth|IPI|timer' | head -8; ps | grep -c kvlat; dmesg | grep -E 'BUG|WARN|hrtimer|rcu|lockup|stall|took|machrev: (timer|revision)|riscv-timer|sched_clock|clocksource' | head -40", 120)
        ok = True
    except Exception as e:
        out = q.output_since(m)[-1500:]; ok = False
    G.detach(q)
    if not ok or why:
        print("ALIVE? %s after %s:\n%s" % (ok, why, out)); sys.stdout.flush()
    return ok

def run_load(q, vmid, fact):
    C.send("kvlat load -c %s -r %d -t %d -b 100 > /tmp/kv.out &\n" % (TARGET, RATE, SECS))
    C.expect(r"\$ ", 30)
    time.sleep(5)
    t_ask = None
    if fact:
        q.send("vmm revise %d %s\n" % (vmid, fact))
        q.expect(r"\$ ", 120)
        t_ask = 5.0
    time.sleep(SECS - 5 + 6)
    for tries in range(7):
        m = C.mark()
        C.send("cat /tmp/kv.out\n")
        C.expect(r"\$ ", 60)
        out = C.output_since(m)
        if "kvlat total" in out:
            break
        print("  (no total yet, %d s late: %d bins so far)" % (5 * tries, out.count("kvlat bin"))); sys.stdout.flush()
        time.sleep(5)
    if "kvlat total" not in out:
        print("  load output:\n" + out[-1500:]); sys.stdout.flush()
    bins = [(float(a), int(n), int(p50), int(p99), int(mx), int(e)) for a, n, p50, p99, mx, e in
            re.findall(r"kvlat bin (\S+) n (\d+) p50 (\d+) p99 (\d+) max (\d+) err (\d+)", out)]
    tot = re.search(r"kvlat total (\d+) answered (\d+) errors (\d+) reconnects (\d+) p50 (\d+) p99 (\d+) p999 (\d+) max (\d+)(?: gap (\d+))?", out)
    return bins, tot

def summarise(bins, ask=5.0):
    before = [b for b in bins if 1.0 <= b[0] < ask]
    after = [b for b in bins if b[0] >= ask]
    window = [b for b in after if b[0] < ask + 1.0]      # the second after the revision is asked
    base = statistics.median([b[3] for b in before]) if before else 0
    worst = max(window, key=lambda b: b[4]) if window else None
    back = None                                          # first bin from the ask on that is at the baseline again
    for b in after:
        if b[3] <= 2 * max(base, 1) and b[5] == 0:
            back = b[0] - ask; break
    return base, worst, sum(b[5] for b in after), back

# SPLIT (a fourth argument "split"): the guest on host B and the load generator on host A, joined by
# the private link, so that the client does not take the guest's host's harts (at 500 requests a
# second its poll timeout is 0-1 ms). Otherwise both on one host.
SPLIT = len(sys.argv) > 4 and sys.argv[4] == "split"
rows = []
if SPLIT:
    link = os.path.join(HERE, "tail%d.sock" % os.getpid())
    try: os.unlink(link)
    except OSError: pass
    log = open("tail-console.log", "w")
    C = T.QEMU(intc="imsic", cpus=3, log=log, fsimg="tail-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
    C.expect(r"init: starting sh", 120); C.expect(r"\$ ", 30)
    q = T.QEMU(intc="imsic", cpus=3, log=log, fsimg="tail-b.img", extra_env={"LINK": link, "LINKSRV": "off"})
    q.expect(r"init: starting sh", 120); q.expect(r"\$ ", 30)
    for h, ip in ((C, "10.7.0.1"), (q, "10.7.0.2")):
        h.send("ip addr add %s/24 dev eth0\n" % ip); h.expect(r"\$ ", 30)
    TARGET, GUESTIP = "10.7.0.50", "10.7.0.50/24"
else:
    q = G.boot_host(T.QEMU, "imsic", 3, open("tail-console.log", "w"))
    C = q
    TARGET, GUESTIP = "10.0.2.50", "10.0.2.50/24"
try:
    vmid = L.start_linux(q, "-f /linux.img -c 2 -H -V -i %s -p 6379" % GUESTIP, timeout=300)
    L.linux_cmd(q, "setsid kvlat serve < /dev/null > /tmp/kvs.log 2>&1 &", 60)
    out = L.linux_cmd(q, "sleep 2; cat /tmp/kvs.log; ps | grep -v grep | grep kvlat; cat /proc/net/tcp | head -4; dmesg | tail -5", 60)
    print("server:\n" + out); sys.stdout.flush()
    assert "serving on port" in out, out
    G.detach(q)
    for name, facts in EVENTS:
        for r in range(ROUNDS):
            for fact in facts:
                bins, tot = run_load(q, vmid, fact)
                if not tot:
                    # recorded, and the pass goes on: one stall must not cost the other facts' rows
                    alive(q, fact)
                    print("E5 %-18s %-18s NO TOTAL (the load did not finish)" % (name, fact)); sys.stdout.flush()
                    rows.append((name, fact, 0, None, -1, None, None))
                    time.sleep(40)
                    continue
                base, worst, errs, back = summarise(bins)
                rows.append((name, fact, base, worst, errs, back, tot.groups() if tot else None))
                print("E5 %-18s %-18s base p99 %6d us; worst bin %s: p99 %d max %d us; errors %d; back in %s s; total %s" % (
                    name, fact, base, worst and worst[0], worst and worst[3], worst and worst[4], errs,
                    "%.1f" % back if back is not None else "-", tot and tot.group(0)))
                sys.stdout.flush()
finally:
    q.close()
    if C is not q:
        C.close()
print("\n| event | revision | baseline p99 (us) | worst p99 (us) | worst max (us) | errors | back to baseline (s) |")
print("|---|---|---|---|---|---|---|")
for name, fact, base, worst, errs, back, tot in rows:
    print("| %s | %s | %d | %s | %s | %d | %s |" % (name, fact or "-", base, worst and worst[3], worst and worst[4], errs,
          "%.1f" % back if back is not None else "-"))
