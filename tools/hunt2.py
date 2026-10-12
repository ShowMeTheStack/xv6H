# A guest migrated back and forth between two hosts of different shapes, with a
# correctness check on every arrival. Each crossing exchanges its interrupt
# controller, so this is N controller rebinds with real disk work between them.
#
#   python3 tools/hunt2.py [cycles]      # one cycle = A->B->A
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
sys.path.insert(0, os.getcwd())
import test_hyp_snap as sn

CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 else 25
tag = "loop%d" % os.getpid()
link = os.path.join(os.getcwd(), tag + ".sock")
try: os.unlink(link)
except OSError: pass
LOGA = open("consoleA.log", "w"); LOGB = open("consoleB.log", "w")
# every crossing exchanges the controller: the first arrival policy (guestpolicy=native). Under
# the current default a PLIC host keeps an arriving IMSIC guest on the software model and asks
# nothing, so this loop would test no exchange (snap-migrate-hetero-cheapest covers that policy)
a = th.QEMU(intc="imsic", cpus=3, log=LOGA, fsimg=tag+"-a.img",
            extra_env={"LINK": link, "LINKSRV": "on", "APPEND": "guestpolicy=native"})
b = None
lat, bad, n = [], [], 0
try:
    a.expect(r"init: starting sh", 90); a.expect(r"\$ ", 30)
    b = th.QEMU(intc="plic", cpus=3, log=LOGB, fsimg=tag+"-b.img",
                extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "guestpolicy=native"})
    b.expect(r"init: starting sh", 90); b.expect(r"\$ ", 30)
    sn.run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
    sn.run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
    g.start_guest(a, "-n h1 -m 32 -c 1", 120)
    a.send("echo gen0 > /tmp/keep\n"); a.expect(r"\$ ", 30)
    g.detach(a)

    def cross(src, dst, dstip, want, i):
        global n
        dst.send("vmm accept 9000 -d &\n"); dst.expect(r"\$ ", 30)
        sn.run(dst, "sleep 1", 20)
        out = sn.run(src, "vmm migrate h1 %s:9000" % dstip, 300)
        if "migrated to" not in out:
            bad.append("crossing %d: migrate failed: %s" % (i, out.strip()[-120:]))
            return False
        try:
            dst.expect(r"arrived as \w+, this host gives %s" % want, 120)
            dst.expect(r"arrived: 1 vcpu", 120)
            m = dst.expect(r"revision \d+ adopted \(0x[0-9a-f]+\) after (\d+) us", 20)
            lat.append(int(m.group(1)))
        except Exception as e:
            bad.append("crossing %d: %s" % (i, str(e)[:160]))
            try:
                dst.send("\n"); dst.expect(r"\$ ", 20)
                mk = dst.mark(); dst.send("vmm info h1\n"); dst.expect(r"\$ ", 30)
                bad.append("  destination view: " + " | ".join(l.strip()[:120] for l in dst.output_since(mk).splitlines()
                           if any(k in l for k in ("intc", "revision", "exits", "vcpu0:", "vsie", "imsic0", "observed"))))
            except Exception as e2:
                bad.append("  (could not inspect destination: %s)" % str(e2)[:80])
            # does the acknowledgement arrive late, or never?  ask again by hand and time it
            try:
                mk2 = dst.mark(); dst.send("vmm revise h1\n"); dst.expect(r"\$ ", 20)
                t0 = time.time(); dst.expect(r"revision \d+ adopted", 30)
                bad.append("  re-ask by hand: adopted after %.1fs" % (time.time() - t0))
            except Exception as e3:
                bad.append("  re-ask by hand: NO adoption (%s)" % str(e3)[:60])
            return False
        dst.send("\n")
        try: dst.expect(r"\$ ", 30)
        except Exception: pass
        n += 1
        # correctness on arrival: the file survives, and a new write reaches the
        # disk through a controller the driver bound to moments ago
        g.attach(dst, "h1"); dst.send("\n")
        try:
            dst.expect(r"\$ ", 60)
            dst.send("cat /tmp/keep\n"); dst.expect(r"gen0", 40)
            dst.expect(r"\$ ", 30)
            mk = dst.mark()
            dst.send("echo c%d > /tmp/c; sync; cat /tmp/c\n" % i)
            dst.expect(r"c%d" % i, 60); dst.expect(r"\$ ", 30)
            if "kernel is booting" in dst.output_since(mk):
                bad.append("crossing %d: the guest rebooted" % i)
        except Exception as e:
            bad.append("crossing %d: guest not working after arrival: %s" % (i, str(e)[:120]))
            g.detach(dst)
            return False
        g.detach(dst)
        return True

    t0 = time.time()
    for i in range(CYCLES):
        if not cross(a, b, "10.7.0.2", "plic", 2*i):     break
        if not cross(b, a, "10.7.0.1", "imsic", 2*i+1):  break
        if (i+1) % 5 == 0:
            print("  %d cycles, %d crossings, %.0fs elapsed" % (i+1, n, time.time()-t0))
    print("CROSSINGS OK: %d of %d" % (n, 2*CYCLES))
    if lat:
        lat_s = sorted(lat)
        print("adoption us: n=%d min=%d median=%d max=%d mean=%.0f"
              % (len(lat), lat_s[0], lat_s[len(lat_s)//2], lat_s[-1], sum(lat)/len(lat)))
    print("PROBLEMS: %d" % len(bad))
    for x in bad[:8]: print("   ", x)
finally:
    if b: b.close()
    a.close()
    for f in (link, os.path.join(os.getcwd(), tag+"-a.img"), os.path.join(os.getcwd(), tag+"-b.img")):
        try: os.unlink(f)
        except OSError: pass
