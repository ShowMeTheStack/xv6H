#!/usr/bin/env python3
# grandtour (L30): a Linux participating guest crossing A -> B -> C -> A where every hop
# changes a different set of facts at once (B: interrupt numbering and timer source;
# C: hart numbering and timebase; back to A: all four the other way), for CYCLES cycles.
# Every arrival: the host's "adopted" line, then the guest itself (the disk it left with
# mounted, a write, crossck's rounds clean). Built on hunt4.py.
#   python3 tools/grandtour.py [cycles]      # one cycle = three crossings
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
lx = SourceFileLoader("lx", "test_hyp_linux.py").load_module()
sn = SourceFileLoader("sn", "test_hyp_snap.py").load_module()
CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 else 100
tag = "gt%d" % os.getpid()
link = os.path.join(os.getcwd(), tag + ".sock")
try: os.unlink(link)
except OSError: pass
HOSTS = [("A", "10.7.0.1", ""),
         ("B", "10.7.0.2", "guestirq=alt guesttimer=sbi"),
         ("C", "10.7.0.3", "guesthart=4 guesttimebase=1000000")]
logs = [open("console%s.log" % h[0], "w") for h in HOSTS]
qs = []
lat, bad, n = [], [], 0
try:
    for i, (name, ip, append) in enumerate(HOSTS):
        env = {"LINK": link, "LINKSRV": "on" if i == 0 else "off"}
        if append:
            env["APPEND"] = append
        q = th.QEMU(intc="plic", cpus=3, log=logs[i], fsimg=tag + "-%s.img" % name.lower(), extra_env=env)
        q.expect(r"init: starting sh", 120); q.expect(r"\$ ", 30)
        sn.run(q, "ip addr add %s/24 dev eth0" % ip, 30)
        qs.append(q)
    a = qs[0]
    a.send("vmm start -n la -k /Image -r 0x80200000 -m 256 -a console=ttyS0 earlycon=sbi -f /linux.img -c 2 -H\n")
    a.expect(r"vm (\d+) la: ", 20); a.expect(r"linux# ", 120)
    out = lx.linux_cmd(a, "dmesg | grep -c 'machrev: speaking'")
    assert re.search(r"\n1\r?\n", out), out
    lx.linux_cmd(a, "mount /dev/vda /mnt && echo gen0 > /mnt/keep && sync && echo written")
    lx.linux_cmd(a, "crossck 64 100000 > /tmp/ck.log 2>&1 &", 20)
    g.detach(a)

    def cross(si, di, i):
        global n
        src, dst = qs[si], qs[di]
        dstip = HOSTS[di][1]
        dst.send("vmm accept 9000 -d &\n"); dst.expect(r"\$ ", 30)
        sn.run(dst, "sleep 1", 20)
        mk = dst.mark()
        out = sn.run(src, "vmm migrate la %s:9000" % dstip, 300)
        if "migrated to" not in out:
            bad.append("crossing %d %s->%s: migrate failed: %s" % (i, HOSTS[si][0], HOSTS[di][0], out.strip()[-120:])); return False
        try:
            dst.expect(r"arrived: 2 vcpu", 120)
            m = dst.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\) after (\d+) us", 90)
            lat.append(int(m.group(2)))
        except Exception as e:
            bad.append("crossing %d %s->%s: %s" % (i, HOSTS[si][0], HOSTS[di][0], str(e)[:160]))
            try:
                dst.send("\n"); dst.expect(r"\$ ", 20)
                mk2 = dst.mark(); dst.send("vmm info la\n"); dst.expect(r"\$ ", 30)
                bad.append("  destination view: " + " | ".join(l.strip()[:140] for l in dst.output_since(mk2).splitlines()
                           if any(k in l for k in ("revision", "exits", "vcpu0:", "vcpu1:", "not acknowledged"))))
                g.attach(dst, "la"); dst.send("\n"); dst.expect(r"linux# ", 20)
                out = lx.linux_cmd(dst, "dmesg | tail -25; cat /proc/interrupts", 60)
                bad.append("  guest view:\n" + "\n".join("    " + l.strip()[:150] for l in out.splitlines() if l.strip()))
                g.detach(dst)
            except Exception as e2:
                bad.append("  (could not inspect: %s)" % str(e2)[:80])
            return False
        told = len(re.findall(r"hyp: vm \d+: arrived with", dst.output_since(mk)))
        dst.send("\n")
        try: dst.expect(r"\$ ", 30)
        except Exception: pass
        n += 1
        g.attach(dst, "la"); dst.send("\n")
        try:
            dst.expect(r"linux# ", 60)
            out = lx.linux_cmd(dst, "cat /mnt/keep; echo c%d > /mnt/c; sync; cat /mnt/c; grep -c virtio /proc/interrupts; nproc; "
                                    "sleep 1; tail -1 /tmp/ck.log; grep -E 'wrong|went back|changed|FAILED|advanced' /tmp/ck.log || echo crossck-clean" % i, 90)
            out = "\n".join(l for l in out.splitlines() if not re.match(r"\[ *\d+\.\d+\] ", l.strip()))
            if not re.search(r"\ngen0\r?\nc%d\r?\n[1-9]" % i, out) or "crossck-clean" not in out or not re.search(r"crossck: round \d+ ok=yes", out):
                bad.append("crossing %d %s->%s: guest check failed: %s" % (i, HOSTS[si][0], HOSTS[di][0], out[-200:]))
                g.detach(dst); return False
        except Exception as e:
            bad.append("crossing %d %s->%s: guest not working after arrival: %s" % (i, HOSTS[si][0], HOSTS[di][0], str(e)[:120]))
            g.detach(dst); return False
        g.detach(dst)
        return True

    t0 = time.time()
    for c in range(CYCLES):
        ok = cross(0, 1, 3 * c) and cross(1, 2, 3 * c + 1) and cross(2, 0, 3 * c + 2)
        if not ok:
            break
        if (c + 1) % 5 == 0:
            print("  %d cycles, %d crossings, %.0fs elapsed" % (c + 1, n, time.time() - t0)); sys.stdout.flush()
    print("CROSSINGS OK: %d of %d" % (n, 3 * CYCLES))
    if lat:
        s = sorted(lat); print("adoption us: n=%d min=%d median=%d p90=%d max=%d" % (len(s), s[0], s[len(s)//2], s[int(0.9*len(s))], s[-1]))
    print("PROBLEMS: %d" % len(bad))
    for x in bad[:12]: print("   ", x)
finally:
    for q in qs:
        try: q.close()
        except Exception: pass
    for f in [link] + [os.path.join(os.getcwd(), tag + "-%s.img" % h[0].lower()) for h in HOSTS]:
        try: os.unlink(f)
        except OSError: pass
