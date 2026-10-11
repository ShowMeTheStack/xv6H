#!/usr/bin/env python3
# heldline (R4b, the re-assertion arm of Theorem 1): a line asserted when the interrupt controller is
# exchanged is delivered by the new controller. In normal running the asserted set is empty at the
# exchange, so the one device the host drives itself is used: the balloon raises a configuration-
# change interrupt when it is asked for memory, and the level stays up until the guest services the
# device. Each round: pause the guest, ask it for memory back (`vmm balloon NAME MB'), exchange the
# controller (`vmm revise NAME KIND'), resume, and check that the host reported the line held at the
# exchange ("N line(s) held"), that the revision was adopted, and that the guest serviced the
# balloon afterwards (the balloon's given-back figure moves). The controller alternates PLIC <->
# APLIC-MSI+IMSIC, N exchanges in all, on an IMSIC host with an xv6 guest (1 vcpu, 64 MB).
#   python3 tools/heldline.py [exchanges]
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))   # run as run_script.py at the tree's root
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 16
LOG = open("console.log", "w")
q = th.QEMU(intc="imsic", cpus=3, log=LOG)

def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)

def given_back(name):
    m = re.search(r"balloon: (\d+) MB asked for, (\d+) MB given back", host("vmm info %s" % name))
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)

ok = held = 0
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    g.start_guest(q, "-n hl -m 64 -c 1", 120)
    g.detach(q)
    kind = "imsic"
    for r in range(N):
        kind = "plic" if kind == "imsic" else "imsic"
        want = 8 if r % 2 == 0 else 0                     # alternately ask for 8 MB back and return it
        host("vmm pause hl")
        host("vmm balloon hl %d" % want)
        m = q.mark()
        out = host("vmm revise hl %s" % kind, 60)
        host("vmm resume hl")
        try:
            q.consumed = m; q.expect(r"revision \d+ adopted", 60); q.consumed = len(q.buf)
            adopted = True
        except Exception:
            adopted = False
        time.sleep(3)
        lines = re.search(r"interrupt controller \S+ -> \S+, (\d+) line\(s\) held", q.output_since(m))
        nheld = int(lines.group(1)) if lines else -1
        asked, gave = given_back("hl")
        serviced = gave == want
        res = adopted and nheld > 0 and serviced
        ok += res; held += nheld > 0
        print("exchange %2d -> %-5s: %d line(s) held at the exchange, adopted %s, balloon %d MB asked, %d MB given back -> %s" % (
            r + 1, kind, nheld, "yes" if adopted else "NO", asked, gave, "ok" if res else "FAIL"))
        sys.stdout.flush()
    print("RESULT %d of %d exchanges with a line held, delivered by the new controller and serviced (held in %d)" % (ok, N, held))
finally:
    q.close()
