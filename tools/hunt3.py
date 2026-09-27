# hunt3: a two-hart *legacy* guest (norevise) crossing back and forth between
# two hosts of the same shape, `sleep 1' after every arrival. The regression run
# saw the legacy two-hart arrival hang at `sleep 1' now and then (ipi/ram/sbi/
# timebase-legacy, all 2 vcpus, all intermittent); this reproduces it and, when
# it does, dumps `vmm info' from the host so the hang is seen from outside.
#   python3 hunt3.py [cycles]
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
sys.path.insert(0, os.getcwd())
import test_hyp_snap as sn

CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 else 50
tag = "h3%d" % os.getpid()
link = os.path.join(os.getcwd(), tag + ".sock")
try: os.unlink(link)
except OSError: pass
LOGA = open("consoleA.log", "w"); LOGB = open("consoleB.log", "w")
a = th.QEMU(intc="plic", cpus=3, log=LOGA, fsimg=tag+"-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
b = None
bad, n, lat = [], 0, []
try:
    a.expect(r"init: starting sh", 90); a.expect(r"\$ ", 30)
    b = th.QEMU(intc="plic", cpus=3, log=LOGB, fsimg=tag+"-b.img", extra_env={"LINK": link, "LINKSRV": "off"})
    b.expect(r"init: starting sh", 90); b.expect(r"\$ ", 30)
    sn.run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
    sn.run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
    g.start_guest(a, "-n ax -m 32 -c 2 -a norevise", 120)
    a.send("echo gen0 > /keep; sync\n"); a.expect(r"\$ ", 30)
    g.detach(a)

    def cross(src, dst, dstip, i):
        global n
        dst.send("vmm accept 9000 -d &\n"); dst.expect(r"\$ ", 30)
        sn.run(dst, "sleep 1", 20)
        out = sn.run(src, "vmm migrate ax %s:9000" % dstip, 300)
        if "migrated to" not in out:
            bad.append("crossing %d: migrate failed: %s" % (i, out.strip()[-160:]))
            # the wire, from both ends, as it is at the stall
            for who, h in (("src", src), ("dst", dst)):
                try:
                    print("---- %s /proc/net/tcp at the stall (crossing %d) ----" % (who, i))
                    print(sn.run(h, "cat /proc/net/tcp; vmm ls", 20))
                except Exception as e:
                    print("  (%s did not answer: %s)" % (who, e))
            sys.stdout.flush()
            return False
        dst.expect(r"arrived: 2 vcpu", 120)
        dst.send("\n"); dst.expect(r"\$ ", 20)
        g.attach(dst, "ax")
        dst.send("\n"); dst.expect(r"\$ ", 30)
        t0 = time.time()
        dst.send("echo c%d > /c; sync; cat /c; sleep 1; echo SLEPT\n" % i)
        try:
            dst.expect(r"SLEPT", 45); dst.expect(r"\$ ", 10)
            lat.append(time.time() - t0)
        except Exception:
            bad.append("crossing %d: sleep 1 did not return in 45 s" % i)
            g.detach(dst)
            info = sn.run(dst, "vmm info ax", 30)
            print("---- vmm info at the hang (crossing %d) ----" % i); print(info)
            time.sleep(3)
            info2 = sn.run(dst, "vmm info ax", 30)
            print("---- 3 s later ----"); print(info2)
            return False
        g.detach(dst)
        n += 1
        return True

    for i in range(CYCLES):
        if not cross(a, b, "10.7.0.2", 2*i+1): break
        if not cross(b, a, "10.7.0.1", 2*i+2): break
        if i % 5 == 4:
            print("  %d crossings ok, sleep-1 round trip median %.2f s" % (n, sorted(lat)[len(lat)//2])); sys.stdout.flush()
finally:
    print("crossings ok: %d; problems: %d" % (n, len(bad)))
    for x in bad: print("  " + x)
    try:
        if b: b.close()
        a.close()
    except Exception: pass
