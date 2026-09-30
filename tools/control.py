#!/usr/bin/env python3
# control (R8, re-taken for L13): without the revision the guest is deaf.
#
# The same crossing twice -- an xv6 guest on an IMSIC host moved to a PLIC host --
# once with the destination told `norevise=1' (it takes the machine and says nothing)
# and once by default (it asks the revision on arrival). What is compared is whether
# the guest answers afterwards: a command typed at its shell, with a bounded wait.
# Prints the two verdicts and the host lines behind them, N times each, interleaved.
#   python3 tools/control.py [N]
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
LOG = open("console.log", "w")

def run(q, line, t=60):
    m = q.mark(); q.send(line + "\n"); q.expect(r"\$ ", t); return q.output_since(m)

def crossing(control):
    tag = "ctl%d%s" % (os.getpid(), "c" if control else "t")
    link = os.path.join(th.HERE, tag + ".sock")
    try: os.unlink(link)
    except OSError: pass
    a = th.QEMU(intc="imsic", cpus=3, log=LOG, fsimg=tag + "-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 90); a.expect(r"\$ ", 30)
        env = {"LINK": link, "LINKSRV": "off"}
        if control:
            env["APPEND"] = "norevise=1"
        b = th.QEMU(intc="plic", cpus=3, log=LOG, fsimg=tag + "-b.img", extra_env=env)
        b.expect(r"init: starting sh", 90); b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30); run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        assert "2 received" in run(a, "ping 10.7.0.2 2", 60)
        g.start_guest(a, "-n h1 -m 32 -c 1", 120)
        a.send("echo before-the-move > /tmp/keep\n"); a.expect(r"\$ ", g.ATONCE)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n"); b.expect(r"\$ ", 30); run(b, "sleep 1", 20)
        out = run(a, "vmm migrate h1 10.7.0.2:9000", 300)
        assert "migrated to 10.7.0.2:9000" in out, out
        m = b.mark()
        b.expect(r"arrived: 1 vcpu", 120)
        time.sleep(2)
        hostlines = [l.strip() for l in b.output_since(m).splitlines() if "hyp: vm" in l]
        # the question: does the guest answer?
        g.attach(b, "h1")
        b.send("\n")
        t0 = time.time()
        try:
            b.expect(r"\$ ", 20)
            b.send("cat /tmp/keep\n")
            b.expect(r"before-the-move", 20)
            b.expect(r"\$ ", 10)
            verdict = "alive (answered in %.1f s, file intact)" % (time.time() - t0)
        except Exception:
            verdict = "wedged (no answer in 20 s)"
        # what the host holds for a guest that cannot be interrupted
        try:
            g.detach(b)
        except Exception:
            pass
        m = b.mark(); b.send("\n"); b.expect(r"\$ ", 10)
        undelivered = re.findall(r"frame nobody asked for, not delivered \((\d+) times\)", b.output_since(m) + "\n".join(hostlines))
        return verdict, hostlines, undelivered[-1] if undelivered else "0"
    finally:
        for q in (a, b):
            if q is not None:
                try: q.close()
                except Exception: pass
        for f in (tag + "-a.img", tag + "-b.img", link):
            try: os.unlink(os.path.join(th.HERE, f))
            except OSError: pass

for i in range(N):
    for control in (True, False):
        v, lines, nd = crossing(control)
        print("######## %s (round %d)" % ("CONTROL: norevise=1" if control else "TREATMENT: default", i + 1))
        for l in lines[:6]:
            print(l)
        print("GUEST: %s; frames held for it: %s" % (v, nd))
        print("VERDICT: %s" % ("wedged" if v.startswith("wedged") else "alive"))
        sys.stdout.flush()
