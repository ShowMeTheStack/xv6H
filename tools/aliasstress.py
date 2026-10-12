# aliasstress: the alias window under fire and its cost. One host,
# the aliasguest payload (2 vcpus; hart 1 hammers the disk's old and new
# windows and zeroes pages with cbo.zero; hart 0 keeps every revision's window
# open for 2 ms before acknowledging). N revisions of the device map, every
# fourth pair also a cache-block revision. Reports the guest's counters and the
# host's lookup costs.
#   python3 tools/aliasstress.py [revisions]
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 200
LOG = open("console.log", "w")
q = th.QEMU(intc="plic", cpus=3, log=LOG)
def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)
def revise(vmid, words):
    q.send("vmm revise %d %s\n" % (vmid, words))
    mm = q.expect(r"revision \d+ adopted \(0x[0-9a-f]+\) after (\d+) us", 60)
    q.send("echo _rs_\n"); q.expect(r"\n_rs_\r?\n", 60); q.expect(r"\$ ", 60)
    return int(mm.group(1)) / 1000.0
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    q.send("vmm start -k /aliasguest -f /disk0.img -c 2 -m 32\n")
    m = q.expect(r"vm (\d+)[^:]*: (\d+) vcpu", 30); vmid = int(m.group(1))
    q.expect(r"aliasguest: revision protocol version 1, ready", 60)
    g.detach(q)
    lat = []; t0 = time.time()
    for i in range(N):
        lat.append(revise(vmid, "map alt" if i % 2 == 0 else "map default"))
        if i % 8 == 3: lat.append(revise(vmid, "cboz 128"))
        if i % 8 == 7: lat.append(revise(vmid, "cboz 64"))
        if (i + 1) % 50 == 0:
            print("  %d map revisions, %.0fs" % (i + 1, time.time() - t0)); sys.stdout.flush()
    info = host("vmm info %d" % vmid, 30)
    look = re.search(r"device lookups: .*", info)
    g.attach(q, str(vmid)); q.send("s")          # a keypress: the guest reports
    q.expect(r"aliasguest: revisions", 30)      # the periodic reports replayed from the backlog first...
    mk = q.mark(); q.send("s"); time.sleep(1)     # ... then the one for the keypress
    q.expect(r"aliasguest: revisions (\d+) reads cur (\d+) old (\d+) errors (\d+)([^\n]*)cboz rounds (\d+) errors (\d+)", 30)
    time.sleep(1)                                 # the keypress report is the last one; the backlog replays the periodic ones first
    mm = list(re.finditer(r"aliasguest: revisions (\d+) reads cur (\d+) old (\d+) errors (\d+)([^\n]*)cboz rounds (\d+) errors (\d+)", q.buf.decode(errors="replace")))[-1]
    first = mm.group(5).decode(errors="replace") if isinstance(mm.group(5), bytes) else mm.group(5)
    g.detach(q)
    s = sorted(lat)
    d = lambda x: x.decode(errors="replace") if isinstance(x, bytes) else x
    print("REVISIONS %s; guest reads: new window %s, old window (during the 2 ms) %s, WRONG READS %s%s; cbo.zero pages %s, WRONG PAGES %s" % (d(mm.group(1)), d(mm.group(2)), d(mm.group(3)), d(mm.group(4)), first.strip(), d(mm.group(6)), d(mm.group(7))))
    print("adoption ms: n=%d median %.1f p90 %.1f max %.1f (2 ms of it deliberate)" % (len(s), s[len(s)//2], s[int(0.9*len(s))], s[-1]))
    print("host: %s" % (look.group(0) if look else info[-300:]))
finally:
    q.close()
