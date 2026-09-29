# obstime (R1, re-taken for L13; L28 for Linux): what a guest observes, and when. The
# host stamps the first observation of every fact class (vmm info: "observed:
# name@Nms"); the guest is sampled after boot, then idle, after disk work, after a
# vector program, after fork-heavy work. Runs xv6 or Linux.
#   python3 tools/obstime.py [xv6|linux]
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
lx = SourceFileLoader("lx", "test_hyp_linux.py").load_module()
KIND = sys.argv[1] if len(sys.argv) > 1 else "xv6"
LOG = open("console.log", "w")
q = th.QEMU(intc="imsic", cpus=3, log=LOG)
def host(cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)
def sample(vmid, label):
    info = host("vmm info %d" % vmid)
    m = re.search(r"observed: ([^\n]*)", info)
    print("%-26s %s" % (label, m.group(1).strip() if m else "(nothing observed)"))
try:
    q.expect(r"init: starting sh", 90); q.expect(r"\$ ", 30)
    t0 = time.time()
    if KIND == "xv6":
        vmid = g.start_guest(q, "-c 1 -m 64", 120)
        run = lambda c, t=120: (q.send(c + "\n"), q.expect(r"\$ ", t))
        prompt = r"\$ "
    else:
        vmid = lx.start_linux(q, "-f /linux.img -c 2 -H")
        run = lambda c, t=120: lx.linux_cmd(q, c, t)
    g.detach(q); sample(vmid, "after boot")
    time.sleep(3); sample(vmid, "idle +3s")
    g.attach(q, str(vmid)); q.send("\n")
    if KIND == "xv6": q.expect(r"\$ ", 30); run("fsbench 4 > /dev/null 2>&1; sync")
    else: q.expect(r"linux# ", 30); run("mount /dev/vda /mnt 2>/dev/null; dd if=/dev/zero of=/mnt/f bs=1M count=4 2>/dev/null; sync")
    g.detach(q); sample(vmid, "after disk work")
    time.sleep(3); sample(vmid, "idle +3s")
    g.attach(q, str(vmid)); q.send("\n")
    if KIND == "xv6": q.expect(r"\$ ", 30); run("vecbench 2")
    else: q.expect(r"linux# ", 30); run("vecuser use 2")
    g.detach(q); sample(vmid, "after a vector program")
    time.sleep(5); sample(vmid, "idle +5s")
    g.attach(q, str(vmid)); q.send("\n")
    if KIND == "xv6": q.expect(r"\$ ", 30); run("schedbench > /dev/null 2>&1")
    else: q.expect(r"linux# ", 30); run("for i in 1 2 3 4 5 6 7 8 9 10; do (true &); done; wait; date > /dev/null; cat /sys/class/rtc/rtc0/time")
    g.detach(q); sample(vmid, "after fork-heavy work")
    if KIND == "linux":
        g.attach(q, str(vmid)); q.send("\n"); q.expect(r"linux# ", 30)
        run("ip link set eth0 up 2>/dev/null; cat /sys/class/net/eth0/address; echo 1 > /proc/sys/vm/drop_caches")
        g.detach(q); sample(vmid, "after net and rtc use")
    print("elapsed %.1f s" % (time.time() - t0))
finally:
    q.close()
