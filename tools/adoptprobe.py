# adoptprobe: adoption latency per fact. One host, one guest (xv6 or Linux);
# each revisable fact is revised there-and-back N times and the host's
# `revision G adopted (0xM) after T us' is collected. Output: per fact,
# n / min / median / p90 / max in ms, and the adopted masks seen.
#   python3 adoptprobe.py xv6 20      python3 adoptprobe.py linux 20
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
lx = SourceFileLoader("lx", "test_hyp_linux.py").load_module()
kind = sys.argv[1] if len(sys.argv) > 1 else "xv6"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 20
# fact -> the two `vmm revise VM ...' words that go there and back
FACTS = [
    ("F5 timer", "timer sbi", "timer sstc"),
    ("F6 timebase", "timebase 1000000", "timebase 10000000"),
    ("F7/F8 cpus", "cpus 1", "cpus 2"),
    ("F9 hartid", "hartid 4", "hartid 0"),
    ("F13 ram (grow only)", None, None),
    ("F16/F17 rtc", "rtc off", "rtc on"),
    ("F18 console", "console virtio", "console uart"),
    ("F19 mac", "mac alt", "mac default"),
    ("F21 cboz", "cboz 128", "cboz 64"),
    ("F22 sbi", "sbi rfence", "sbi none"),
]
if kind == "xv6":
    FACTS = [("F1 intc", "plic", "imsic"), ("F2 intc-addr", "intcmap alt", "intcmap default"),
             ("F3 irqmap", "irqmap alt", "irqmap default"), ("F4 ipi", "ipi sbi", "ipi clint")] + FACTS
if len(sys.argv) > 3:       # only these facts (a word of the name), e.g. cpus,rtc
    only = sys.argv[3].split(",")
    FACTS = [f for f in FACTS if any(w in f[0] for w in only)]
LOG = open("console.log", "w")
def load():
    try: return open("/proc/loadavg").read().split()[0]
    except Exception: return "?"
print("box load at start: %s" % load()); sys.stdout.flush()
# an IMSIC host offers all three controllers (F1 there and back); the others are the same on any
q = th.QEMU(intc="imsic" if kind == "xv6" else "plic", cpus=3, log=LOG)
res = {}
def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    if kind == "xv6":
        vmid = g.start_guest(q, "-c 2 -m 64")
    else:
        vmid = lx.start_linux(q, "-f none -c 2 -H")
    g.detach(q)
    for name, there, back in FACTS:
        if there is None:
            continue
        lat, masks = [], set()
        for i in range(N):
            for words in (there, back):
                m = q.mark()
                q.send("vmm revise %d %s\n" % (vmid, words))
                # the host's `adopted' line and the shell's next prompt come in
                # either order (the guest adopts in milliseconds; the prompt is
                # printed when the command returns): wait for the line, then
                # resync on a fresh prompt -- waiting for the prompt first ate
                # the line half the time and cost 40 s per miss
                try:
                    mm = q.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\) after (\d+) us", 40)
                    lat.append(int(mm.group(2)) / 1000.0); masks.add(mm.group(1).decode() if isinstance(mm.group(1), bytes) else mm.group(1))
                except Exception as e:
                    lat.append(float("nan")); masks.add("timeout")
                q.send("\n"); q.expect(r"\$ ", 60)
                # the cpus revision parks harts: let them settle
                if words.startswith("cpus"):
                    host("sleep 3", 20)
        good = sorted(x for x in lat if x == x)
        res[name] = (len(lat), good, masks)
        print("  (box load now %s)" % load())
        if good:
            print("%-22s n=%-3d ok=%-3d min %7.1f  median %7.1f  p90 %7.1f  max %7.1f ms  adopted %s" % (
                name, len(lat), len(good), good[0], good[len(good)//2], good[min(len(good)-1, int(0.9*len(good)))], good[-1], ",".join(sorted(masks))))
        else:
            print("%-22s n=%d none adopted (%s)" % (name, len(lat), ",".join(sorted(masks))))
        sys.stdout.flush()
finally:
    q.close()
