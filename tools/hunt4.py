# hunt4: a Linux participating guest crossing between two hosts that differ in
# interrupt numbering (the axis on which a revision once went unacknowledged
# for 865 s), with the at-least-once announcement and the revision watchdog on the
# host. Every arrival: the host's "adopted" line, then the guest itself (the
# disk it left with mounted, a write, /proc/interrupts naming the virtio lines).
#   python3 tools/hunt4.py [cycles]      # one cycle = A->B->A
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
lx = SourceFileLoader("lx", "test_hyp_linux.py").load_module()
sys.path.insert(0, os.getcwd())
import test_hyp_snap as sn
CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 else 25
tag = "h4%d" % os.getpid()
link = os.path.join(os.getcwd(), tag + ".sock")
try: os.unlink(link)
except OSError: pass
LOGA = open("consoleA.log", "w"); LOGB = open("consoleB.log", "w")
a = th.QEMU(intc="plic", cpus=3, log=LOGA, fsimg=tag+"-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
b = None
lat, bad, n, repeats = [], [], 0, 0
try:
    a.expect(r"init: starting sh", 90); a.expect(r"\$ ", 30)
    b = th.QEMU(intc="plic", cpus=3, log=LOGB, fsimg=tag+"-b.img",
                extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "guestirq=alt"})
    b.expect(r"init: starting sh", 90); b.expect(r"\$ ", 30)
    sn.run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
    sn.run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
    a.send("vmm start -n la -k /Image -r 0x80200000 -m 256 -a console=ttyS0 earlycon=sbi -f /linux.img -c 2 -H\n")
    a.expect(r"vm (\d+) la: ", 20); a.expect(r"linux# ", 120)
    out = lx.linux_cmd(a, "dmesg | grep -c 'machrev: speaking'")
    assert re.search(r"\n1\r?\n", out), out
    lx.linux_cmd(a, "mount /dev/vda /mnt && echo gen0 > /mnt/keep && sync && echo written")
    # memory, clock and vector file verified from inside the guest every second, for the whole hunt
    lx.linux_cmd(a, "crossck 64 100000 > /tmp/ck.log 2>&1 &", 20)
    g.detach(a)
    def cross(src, dst, dstip, want, i):
        global n, repeats
        dst.send("vmm accept 9000 -d &\n"); dst.expect(r"\$ ", 30)
        sn.run(dst, "sleep 1", 20)
        out = sn.run(src, "vmm migrate la %s:9000" % dstip, 300)
        if "migrated to" not in out:
            bad.append("crossing %d: migrate failed: %s" % (i, out.strip()[-120:])); return False
        try:
            dst.expect(r"arrived with its interrupt lines numbered \w+, this host numbers them %s" % want, 120)
            dst.expect(r"arrived: 2 vcpu", 120)
            m = dst.expect(r"revision \d+ adopted \(0x[0-9a-f]+\) after (\d+) us", 60)
            lat.append(int(m.group(1)))
        except Exception as e:
            bad.append("crossing %d: %s" % (i, str(e)[:160]))
            try:
                dst.send("\n"); dst.expect(r"\$ ", 20)
                mk = dst.mark(); dst.send("vmm info la\n"); dst.expect(r"\$ ", 30)
                bad.append("  destination view: " + " | ".join(l.strip()[:140] for l in dst.output_since(mk).splitlines()
                           if any(k in l for k in ("revision", "exits", "vcpu0:", "vcpu1:", "not acknowledged", "co-run"))))
            except Exception as e2:
                bad.append("  (could not inspect: %s)" % str(e2)[:80])
            # the guest's side, if it will still talk (the other vcpu may be free)
            try:
                g.attach(dst, "la"); dst.send("\n"); dst.expect(r"linux# ", 20)
                out = lx.linux_cmd(dst, "dmesg | tail -25; cat /proc/interrupts", 60)
                bad.append("  guest view:\n" + "\n".join("    " + l.strip()[:150] for l in out.splitlines() if l.strip()))
                g.detach(dst)
            except Exception as e3:
                bad.append("  (guest did not answer: %s)" % str(e3)[:80])
            return False
        dst.send("\n")
        try: dst.expect(r"\$ ", 30)
        except Exception: pass
        mk = dst.mark(); dst.send("vmm info la\n"); dst.expect(r"\$ ", 30)
        mm = re.search(r"announcement repeated (\d+)", dst.output_since(mk))
        if mm: repeats = int(mm.group(1))
        n += 1
        g.attach(dst, "la"); dst.send("\n")
        try:
            dst.expect(r"linux# ", 60)
            out = lx.linux_cmd(dst, "cat /mnt/keep; echo c%d > /mnt/c; sync; cat /mnt/c; grep -c virtio /proc/interrupts; "
                                    "sleep 1; tail -1 /tmp/ck.log; grep -E 'wrong|went back|changed|FAILED|advanced' /tmp/ck.log || echo crossck-clean" % i, 90)
            # a kernel line may land between the answers (e.g.
            # "hrtimer: interrupt took 401347500 ns", printed for the time the machine stood still)
            out = "\n".join(l for l in out.splitlines() if not re.match(r"\[ *\d+\.\d+\] ", l.strip()))
            if not re.search(r"\ngen0\r?\nc%d\r?\n[1-9]" % i, out) or "crossck-clean" not in out or not re.search(r"crossck: round \d+ ok=yes", out):
                bad.append("crossing %d: guest check failed: %s" % (i, out[-200:]))
                g.detach(dst); return False
        except Exception as e:
            bad.append("crossing %d: guest not working after arrival: %s" % (i, str(e)[:120]))
            g.detach(dst); return False
        g.detach(dst)
        return True
    t0 = time.time()
    for i in range(CYCLES):
        if not cross(a, b, "10.7.0.2", "alt", 2*i): break
        if not cross(b, a, "10.7.0.1", "default", 2*i+1): break
        if (i+1) % 5 == 0:
            print("  %d cycles, %d crossings, %.0fs elapsed, announcements repeated %d" % (i+1, n, time.time()-t0, repeats)); sys.stdout.flush()
    print("CROSSINGS OK: %d of %d" % (n, 2*CYCLES))
    if lat:
        s = sorted(lat); print("adoption us: n=%d min=%d median=%d p90=%d max=%d" % (len(s), s[0], s[len(s)//2], s[int(0.9*len(s))], s[-1]))
    print("ANNOUNCEMENTS REPEATED: %d" % repeats)
    print("PROBLEMS: %d" % len(bad))
    for x in bad[:12]: print("   ", x)
finally:
    if b: b.close()
    a.close()
    for f in (link, os.path.join(os.getcwd(), tag+"-a.img"), os.path.join(os.getcwd(), tag+"-b.img")):
        try: os.unlink(f)
        except OSError: pass
