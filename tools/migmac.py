#!/usr/bin/env python3
# migmac: the migration stream authenticated (HMAC-SHA256, `migkey=' on both hosts' command lines).
# Two xv6H hosts on the private link; an xv6 guest of MB megabytes crosses A -> B in each case:
#   none   no key on either side                       moves (the baseline time)
#   same   the same key on both                        moves (the cost of the MAC)
#   wrong  another key on B                            refused: "failed authentication"; runs on at A
#   nokey  a key on A only                             refused by B ("has no key"); runs on at A
# Each case boots its own pair. ROUNDS moves per accepted case (A -> B -> A ...).
#   tools/remote.sh script migmac tools/migmac.py [MB] [ROUNDS]
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(HERE) == "tools":
    HERE = os.path.dirname(HERE)
os.chdir(HERE); sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("testhyp", os.path.join(HERE, "test-hyp.py"))
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
import test_hyp_guest as G
MB = int(sys.argv[1]) if len(sys.argv) > 1 else 64
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 3
CASES = [("none", "", ""), ("same", "migkey=k3y-0f-the-pair", "migkey=k3y-0f-the-pair"),
         ("wrong", "migkey=k3y-0f-the-pair", "migkey=another-key"), ("nokey", "migkey=k3y-0f-the-pair", "")]

def run(q, c, t=60):
    m = q.mark(); q.send(c + "\n"); q.expect(r"\$ ", t); return q.output_since(m)

log = open("migmac-console.log", "w")
for name, ka, kb in CASES:
    link = os.path.join(HERE, "mm%d.sock" % os.getpid())
    try: os.unlink(link)
    except OSError: pass
    a = T.QEMU(intc="imsic", cpus=3, log=log, fsimg="mm-a.img", extra_env={"LINK": link, "LINKSRV": "on", "APPEND": ka})
    b = None
    try:
        a.expect(r"init: starting sh", 120); a.expect(r"\$ ", 30)
        b = T.QEMU(intc="imsic", cpus=3, log=log, fsimg="mm-b.img", extra_env={"LINK": link, "LINKSRV": "off", "APPEND": kb})
        b.expect(r"init: starting sh", 120); b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0"); run(b, "ip addr add 10.7.0.2/24 dev eth0")
        G.start_guest(a, "-n m1 -m %d -c 1" % MB, 180)
        a.send("echo the-guest-%s > /tmp/keep\n" % name); a.expect(r"\$ ", 30)
        G.detach(a)
        here, there, port = a, b, 9000
        for r in range(ROUNDS if name in ("none", "same") else 1):
            run(there, "vmm accept %d -d &" % port); time.sleep(1)
            tm = there.mark()
            t0 = time.time()
            out = run(here, "vmm migrate m1 %s:%d" % ("10.7.0.2" if here is a else "10.7.0.1", port), 900)
            dt = time.time() - t0
            moved = "migrated to" in out
            tl = re.search(r"crossing, this side: stopped \+0, last page \+(\d+) us", out)
            time.sleep(3)
            there.send("\n"); there.expect(r"\$ ", 30)       # the console is read only under expect
            far = there.output_since(tm)
            if not moved:
                print("  destination said: " + " | ".join(l.strip() for l in far.splitlines() if l.strip())[-600:])
            why = re.findall(r"hyp: [^\n]*(?:authenticat|key)[^\n]*", far)
            print("MIGMAC %-5s round %d %s: %s in %.2f s%s%s" % (name, r, "A->B" if here is a else "B->A",
                  "moved" if moved else "NOT moved", dt, "; stop to last page %.0f ms" % (int(tl.group(1)) / 1000) if tl else "",
                  ("; " + why[-1]) if why and not moved else ""))
            sys.stdout.flush()
            if moved:
                G.attach(there, "m1"); there.send("\n"); there.expect(r"\$ ", 60)
                o = run(there, "cat /tmp/keep", 30)
                print("  the guest on the destination: %s" % ("ok" if ("the-guest-%s" % name) in o else "MISSING: " + o[-200:]))
                G.detach(there)
                here, there = there, here
                port = 9001 if port == 9000 else 9000
            else:
                G.attach(here, "m1"); here.send("\n"); here.expect(r"\$ ", 60)
                o = run(here, "cat /tmp/keep", 30)
                print("  the guest still on the source: %s" % ("ok" if ("the-guest-%s" % name) in o else "MISSING: " + o[-200:]))
                G.detach(here)
            sys.stdout.flush()
    finally:
        for q in (a, b):
            if q:
                q.close()
        for f in (link, "mm-a.img", "mm-b.img"):
            try: os.unlink(f)
            except OSError: pass
