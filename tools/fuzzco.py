# fuzzco: a well-behaved machine's adoption latency while a fuzz guest hammers the
# host's emulation and the MACH extension beside it. One host, an xv6
# guest revised N times before the fuzz starts and N times while it runs.
#   python3 tools/fuzzco.py [N] [fuzz rounds]
import os, re, sys, time, random, statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
N = int(sys.argv[1]) if len(sys.argv) > 1 else 20
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 300000
LOG = open("console.log", "w")
q = th.QEMU(intc="plic", cpus=3, log=LOG)
def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)
def revise(vmid, words):
    mk = q.mark()
    q.send("vmm revise %d %s\n" % (vmid, words))
    q.consumed = mk                       # the ack may land before the prompt
    # this machine's line: the fuzz guest acknowledges revisions nobody asked for, by the hundred
    mm = q.expect(r"hyp: vm %d: revision \d+ adopted \(0x[0-9a-f]+\) after (\d+) us" % vmid, 60)
    q.consumed = len(q.buf)
    q.send("echo _rs_\n"); q.expect(r"\n_rs_\r?\n", 60); q.expect(r"\$ ", 60)
    return int(mm.group(1)) / 1000.0
def rounds(vmid, tag):
    lat = []
    for i in range(N):
        lat.append(revise(vmid, "cboz %d" % (128 if i % 2 == 0 else 64)))
    lat.sort()
    print("%s: adoption ms n=%d min %.1f median %.1f p90 %.1f max %.1f" % (tag, N, lat[0], statistics.median(lat), lat[int(0.9 * (N - 1))], lat[-1]))
    return lat
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    vmid = g.start_guest(q, "-c 1 -m 32", 120)
    g.detach(q)
    quiet = rounds(vmid, "before the fuzz")
    seed = random.randrange(1, 2 ** 31)
    q.send('vmm start -k /fuzzguest -f none -m 32 -c 2 -a "seed=%d rounds=%d"\n' % (seed, ROUNDS))
    q.expect(r"fuzzguest: seed", 60)
    g.detach(q)                           # `vmm start' attaches; the host shell is wanted, not the fuzz guest's console
    print("fuzz guest running: seed %d, %d rounds" % (seed, ROUNDS))
    time.sleep(2)
    q.send("\n"); q.expect(r"\$ ", 30)
    loud = rounds(vmid, "under the fuzz")
    info = host("vmm info %d" % vmid)
    mm = re.search(r"exits (\d+):", info)
    print("the fuzz guest's rounds so far: %s" % (re.search(r"fuzzguest: [^\n]*rounds[^\n]*", q.buf.decode(errors="replace")[-4000:]) or "(not printed yet)"))
    q.expect(r"\[vm \d+ has exited\]", 900)     # the fuzz guest exits when its rounds are done
    q.send("\n"); q.expect(r"\$ ", 30)
    print("the fuzz guest finished; the well-behaved machine: %s" % ("alive" if "revision" in host("vmm info %d" % vmid) else "gone"))
    print("RATIO of medians (under/before): %.2f" % (statistics.median(loud) / statistics.median(quiet)))
finally:
    q.close()
