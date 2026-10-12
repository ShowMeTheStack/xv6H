#!/usr/bin/env python3
# grandtour: a Linux participating guest crossing a ring of shapes, A -> B -> C -> A ...,
# where every hop changes a different set of facts (A: the defaults; B: interrupt numbering
# and timer source; C: hart numbering and timebase), for CYCLES cycles of three hops.
#
# The harness's network is one stream socket per host, a two-party link, so three hosts
# cannot see each other at once: the tour runs on two slots. The guest is on one; the
# other is booted in the next shape, the guest crosses to it, and the slot it left is
# rebooted in the shape after that. Slot 0 holds the socket's server side and slot 1 the
# client, which reconnects when the server is restarted (Makefile: reconnect-ms). Every
# arrival: the host's "adopted" line, then the guest itself (the disk it left with
# mounted, a write, crossck's rounds clean, nproc, the printk clock not jumped).
#   python3 tools/grandtour.py [cycles]
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
SHAPES = [("A", ""), ("B", "guestirq=alt guesttimer=sbi"), ("C", "guesthart=4 guesttimebase=1000000")]
IP = ["10.7.0.1", "10.7.0.2"]
LOG = [open("console0.log", "w"), open("console1.log", "w")]
slot = [None, None]
lat, bad, n = [], [], 0

def boot(i, shape):
    """slot i booted in SHAPE (name, append); slot 0 is the link's server"""
    env = {"LINK": link, "LINKSRV": "on" if i == 0 else "off"}
    if shape[1]:
        env["APPEND"] = shape[1]
    q = th.QEMU(intc="plic", cpus=3, log=LOG[i], fsimg=tag + "-%d.img" % i, extra_env=env)
    q.expect(r"init: starting sh", 120); q.expect(r"\$ ", 30)
    sn.run(q, "ip addr add %s/24 dev eth0" % IP[i], 30)
    q.shape = shape[0]
    slot[i] = q
    return q

def reboot(i, shape):
    try: slot[i].close()
    except Exception: pass
    time.sleep(1)
    return boot(i, shape)

try:
    a = boot(0, SHAPES[0]); boot(1, SHAPES[1])
    a.send("vmm start -n la -k /Image -r 0x80200000 -m 256 -a console=ttyS0 earlycon=sbi -f /linux.img -c 2 -H\n")
    a.expect(r"vm (\d+) la: ", 20); a.expect(r"linux# ", 120)
    out = lx.linux_cmd(a, "dmesg | grep -c 'machrev: speaking'")
    assert re.search(r"\n1\r?\n", out), out
    lx.linux_cmd(a, "mount /dev/vda /mnt && echo gen0 > /mnt/keep && sync && echo written")
    lx.linux_cmd(a, "crossck 64 100000 > /tmp/ck.log 2>&1 &", 20)
    g.detach(a)

    def cross(si, di, i):
        global n
        src, dst = slot[si], slot[di]
        # the link may still be reconnecting after a reboot: a ping first
        for _ in range(10):
            if "1 received" in sn.run(src, "ping %s 1" % IP[di], 20) or "1 packets received" in sn.run(src, "ping %s 1" % IP[di], 20):
                break
            time.sleep(1)
        dst.send("vmm accept 9000 -d &\n"); dst.expect(r"\$ ", 30)
        sn.run(dst, "sleep 1", 20)
        mk = dst.mark()
        out = sn.run(src, "vmm migrate la %s:9000" % IP[di], 300)
        if "migrated to" not in out:
            bad.append("crossing %d %s->%s: migrate failed: %s" % (i, src.shape, dst.shape, out.strip()[-160:])); return False
        try:
            dst.expect(r"arrived: 2 vcpu", 120)
            m = dst.expect(r"revision \d+ adopted \(0x([0-9a-f]+)\) after (\d+) us", 90)
            lat.append(int(m.group(2)))
        except Exception as e:
            bad.append("crossing %d %s->%s: %s" % (i, src.shape, dst.shape, str(e)[:160]))
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
        dst.send("\n")
        try: dst.expect(r"\$ ", 30)
        except Exception: pass
        n += 1
        g.attach(dst, "la"); dst.send("\n")
        try:
            dst.expect(r"linux# ", 60)
            out = lx.linux_cmd(dst, "cat /mnt/keep; echo c%d > /mnt/c; sync; cat /mnt/c; grep -c virtio /proc/interrupts; nproc; "
                                    "sleep 1; tail -1 /tmp/ck.log; grep -E 'wrong|went back|changed|FAILED|advanced' /tmp/ck.log || echo crossck-clean; "
                                    "dmesg | tail -1" % i, 90)
            out = "\n".join(l for l in out.splitlines() if not re.match(r"\[ *\d+\.\d+\] ", l.strip()) or l is out.splitlines()[-1])
            m = re.search(r"\[\s*(\d+)\.\d+\]", out.splitlines()[-1] if out.splitlines() else "")
            if not re.search(r"\ngen0\r?\nc%d\r?\n[1-9]" % i, out) or "crossck-clean" not in out or not re.search(r"crossck: round \d+ ok=yes", out):
                bad.append("crossing %d %s->%s: guest check failed: %s" % (i, src.shape, dst.shape, out[-200:]))
                g.detach(dst); return False
        except Exception as e:
            bad.append("crossing %d %s->%s: guest not working after arrival: %s" % (i, src.shape, dst.shape, str(e)[:120]))
            g.detach(dst); return False
        g.detach(dst)
        return True

    t0 = time.time()
    here = 0                       # the slot the guest is on
    k = 1                          # the index in SHAPES of the shape the guest is going to
    i = 0
    for c in range(3 * CYCLES):
        there = 1 - here
        if not cross(here, there, i):
            break
        i += 1
        # the slot the guest left is rebooted in the shape after the one it is on
        k = (k + 1) % len(SHAPES)
        try:
            reboot(here, SHAPES[k])
        except Exception as e:
            bad.append("crossing %d: could not reboot slot %d as %s: %s" % (i, here, SHAPES[k][0], str(e)[:120])); break
        here = there
        if (c + 1) % 15 == 0:
            print("  %d crossings, %.0fs elapsed" % (n, time.time() - t0)); sys.stdout.flush()
    print("CROSSINGS OK: %d of %d" % (n, 3 * CYCLES))
    if lat:
        s = sorted(lat); print("adoption us: n=%d min=%d median=%d p90=%d max=%d" % (len(s), s[0], s[len(s)//2], s[int(0.9*len(s))], s[-1]))
    print("PROBLEMS: %d" % len(bad))
    for x in bad[:12]: print("   ", x)
finally:
    for q in slot:
        if q is not None:
            try: q.close()
            except Exception: pass
    for f in (link, os.path.join(os.getcwd(), tag + "-0.img"), os.path.join(os.getcwd(), tag + "-1.img")):
        try: os.unlink(f)
        except OSError: pass
