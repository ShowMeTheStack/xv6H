#!/usr/bin/env python3
# migscale (L14d): migration downtime and total time against guest RAM and dirtying
# rate -- the standard live-migration curves. An xv6 guest crosses from an IMSIC host to
# a PLIC host (the heterogeneous crossing, revised on arrival) while `dirty' rewrites
# half its memory at RATE MB/s. What is read: the migrate command's wall time on the
# source, and both hosts' timeline lines (source: stopped +0, last page, taken, committed;
# destination: first instruction, revision asked, adopted). N crossings per point.
#   python3 tools/migscale.py [N] [mem,mem,...] [rate,rate,...]
import os, re, sys, time, statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
MEMS = [int(x) for x in sys.argv[2].split(",")] if len(sys.argv) > 2 else [64, 256, 1024]
RATES = [int(x) for x in sys.argv[3].split(",")] if len(sys.argv) > 3 else [0, 8, 32]
LOG = open("console.log", "w")
S = re.compile(r"crossing, this side: stopped \+0, last page \+(\d+) us, taken by the far end \+(\d+) us, committed \+(\d+) us")
A = re.compile(r"arrival timeline: stream done \+0, resumed \+(\d+) us, first instruction \+(\d+) us")
R = re.compile(r"arrival timeline: revision asked \+(\d+) us, adopted \+(\d+) us")

def run(q, line, t=60):
    m = q.mark(); q.send(line + "\n"); q.expect(r"\$ ", t); return q.output_since(m)

def crossing(mem, rate):
    tag = "ms%d" % os.getpid()
    link = os.path.join(th.HERE, tag + ".sock")
    try: os.unlink(link)
    except OSError: pass
    hostmem = "%dM" % max(1024, mem * 2 + 512)
    a = th.QEMU(intc="imsic", cpus=3, mem=hostmem, log=LOG, fsimg=tag + "-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 120); a.expect(r"\$ ", 30)
        # guestpolicy=native: the destination revises the arriving guest to its own PLIC, as the
        # first version did by default (the cheapest-controller policy would keep the IMSIC and
        # there would be no revision to time)
        b = th.QEMU(intc="plic", cpus=3, mem=hostmem, log=LOG, fsimg=tag + "-b.img", extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "guestpolicy=native"})
        b.expect(r"init: starting sh", 120); b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0", 30); run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        assert "2 received" in run(a, "ping 10.7.0.2 2", 60)
        g.start_guest(a, "-n h1 -m %d -c 1" % mem, 240)
        half = max(8, mem // 2)
        a.send("dirty %d %d 600 &\n" % (half, rate))
        a.expect(r"dirty: %d MB written" % half, 300)
        a.send("\n"); a.expect(r"\$ ", 30)
        g.detach(a)
        b.send("vmm accept 9000 -d &\n"); b.expect(r"\$ ", 30); run(b, "sleep 1", 20)
        ma, mb = a.mark(), b.mark()
        t0 = time.time()
        out = run(a, "vmm migrate h1 10.7.0.2:9000", 900)
        total = time.time() - t0
        assert "migrated to 10.7.0.2:9000" in out, out[-500:]
        b.expect(r"revision 1 adopted \(0x[0-9a-f]+\) after \d+ us", 120)
        time.sleep(1)
        sa, sb = a.output_since(ma), b.output_since(mb)
        s = S.search(sa); ar = A.search(sb); rv = R.search(sb)
        # the dirtier's verdict after the move
        g.attach(b, "h1"); b.send("\n"); b.expect(r"\$ ", 60)
        b.send("kill %1; sleep 2; echo _k_\n"); b.expect(r"_k_", 60)
        b.expect(r"\$ ", 30)
        g.detach(b)
        return {"total": total,
                "lastpage": int(s.group(1)) / 1000 if s else float("nan"), "committed": int(s.group(3)) / 1000 if s else float("nan"),
                "first": int(ar.group(2)) / 1000 if ar else float("nan"), "adopted": int(rv.group(2)) / 1000 if rv else float("nan")}
    finally:
        for q in (a, b):
            if q is not None:
                try: q.close()
                except Exception: pass
        for f in (tag + "-a.img", tag + "-b.img", link):
            try: os.unlink(os.path.join(th.HERE, f))
            except OSError: pass

print("%-8s %-8s %3s %10s %10s %10s %10s %10s" % ("mem MB", "MB/s", "n", "total s", "last page", "committed", "first insn", "adopted"))
print("%-8s %-8s %3s %10s %10s %10s %10s %10s" % ("", "", "", "med", "ms med", "ms med", "ms med", "ms med"))
for mem in MEMS:
    for rate in RATES:
        rows = []
        for i in range(N):
            try:
                rows.append(crossing(mem, rate))
            except Exception as e:
                print("mem %d rate %d round %d: FAILED: %s" % (mem, rate, i, str(e)[:120])); sys.stdout.flush()
        if rows:
            med = lambda k: statistics.median([r[k] for r in rows if r[k] == r[k]] or [float("nan")])
            print("%-8d %-8d %3d %10.1f %10.0f %10.0f %10.0f %10.0f" % (mem, rate, len(rows), med("total"), med("lastpage"), med("committed"), med("first"), med("adopted")))
            sys.stdout.flush()
