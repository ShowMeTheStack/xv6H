#!/usr/bin/env python3
# scenario: the mechanism in the service of a fleet decision, end to end.
#
#   python3 tools/scenario.py upgrade
#     Four guests (two xv6, two Linux) run on a host whose interrupt controller is a
#     PLIC, each with work of its own. One at a time they are moved to a host built with
#     the AIA and hardware guest interrupt files, where each is revised to an
#     APLIC-MSI+IMSIC on arrival. Per guest, before and after: controller accesses per
#     device interrupt and exits per interrupt; per move: the wall time of the command
#     and the outage from the hosts' timeline lines. What it shows: a fleet can upgrade
#     its hosts under its guests, and what each guest gains by arriving on the better one.
#
#   python3 tools/scenario.py consolidate
#     A Linux guest of 256 MB on a host with room to spare is moved to a host that keeps
#     its guests to 128 MB: the destination asks, the guest's balloon gives the memory
#     back (F14), and the move goes through. Then back to the roomy host, where it is
#     grown again (F13). The guest's and the host's view of its footprint at each step.
import os, re, sys, time
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
lx = SourceFileLoader("lx", "test_hyp_linux.py").load_module()
sn = SourceFileLoader("sn", "test_hyp_snap.py").load_module()
WHICH = sys.argv[1] if len(sys.argv) > 1 else "upgrade"
LOG = open("console.log", "w")
S = re.compile(r"crossing, this side: stopped \+0, last page \+(\d+) us, taken by the far end \+(\d+) us, committed \+(\d+) us")
R = re.compile(r"arrival timeline: revision asked \+(\d+) us, adopted \+(\d+) us")

def info(q, name):
    m = q.mark(); q.send("vmm info %s\n" % name); q.expect(r"\$ ", 60)
    return q.output_since(m)

def counters(text):
    """(controller accesses, device interrupts, exits) from a `vmm info' answer"""
    ctl = 0
    for kind in ("plic", "aplic-s", "aplic-m", "imsic-s", "imsic-m"):
        mm = re.search(r"\b%s@[0-9a-f]+ (\d+)r/(\d+)w" % re.escape(kind), text)
        if mm:
            ctl += int(mm.group(1)) + int(mm.group(2))
    irqs = sum(int(x) for x in re.findall(r"@[0-9a-f]+ \d+r/\d+w/(\d+)i", text))
    mm = re.search(r"exits (\d+):", text)
    return ctl, irqs, int(mm.group(1)) if mm else 0

def resident(text):
    mm = re.search(r"(\d+) MB \((\d+) MB resident\)", text)
    return (int(mm.group(1)), int(mm.group(2))) if mm else (None, None)

def upgrade():
    tag = "up%d" % os.getpid()
    link = os.path.join(os.getcwd(), tag + ".sock")
    try: os.unlink(link)
    except OSError: pass
    # A: a PLIC host (what a fleet has); B: the AIA with hardware guest interrupt files
    a = th.QEMU(intc="plic", cpus=3, mem="3G", log=LOG, fsimg=tag + "-a.img",
                extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    try:
        a.expect(r"init: starting sh", 120); a.expect(r"\$ ", 30)
        b = th.QEMU(intc="imsic", cpus=3, mem="3G", aia_guests=4, log=LOG, fsimg=tag + "-b.img",
                    extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 120); b.expect(r"\$ ", 30)
        sn.run(a, "ip addr add 10.7.0.1/24 dev eth0", 30); sn.run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        guests = []
        for i in (0, 1):
            g.start_guest(a, "-n x%d -m 64 -c 1 -f /disk%d.img" % (i, i), 180)
            a.send("fsbench -q -s 2 -n 20 > /dev/null 2>&1 & echo started\n"); a.expect(r"started", 30)
            a.expect(r"\$ ", 30)
            g.detach(a)
            guests.append("x%d" % i)
        for i, disk in ((0, "/linux.img"), (1, "none")):
            # -V: the description-bound controller (patch 0005), so that this guest takes the
            # destination's controller on arrival instead of keeping its PLIC in software
            a.send("vmm start -n l%d -k /Image -r 0x80200000 -m 192 -a console=ttyS0 earlycon=sbi -f %s -c 1 -H -V\n" % (i, disk))
            a.expect(r"vm (\d+) l%d: " % i, 30); a.expect(r"linux# ", 240)
            lx.linux_cmd(a, "dmesg | grep -c 'machrev: speaking'")
            if disk == "none":
                lx.linux_cmd(a, "(while :; do dd if=/dev/zero of=/dev/null bs=4k count=20000 2>/dev/null; done) & echo started", 30)
            else:
                lx.linux_cmd(a, "mount /dev/vda /mnt; (while :; do dd if=/dev/zero of=/mnt/f bs=64k count=64 2>/dev/null; sync; done) & echo started", 60)
            g.detach(a)
            guests.append("l%d" % i)
        time.sleep(10)
        print("%-6s %22s %22s   %s" % ("guest", "on the PLIC host", "on the AIA host", "the move"))
        print("%-6s %22s %22s   %s" % ("", "acc/irq  exits/irq", "acc/irq  exits/irq", "wall s  outage ms  adopted ms"))
        t0 = time.time()
        for name in guests:
            # before: two readings a few seconds apart, on A
            c0 = counters(info(a, name)); time.sleep(6); c1 = counters(info(a, name))
            dirq = c1[1] - c0[1]
            before = ("%.2f  %.1f" % ((c1[0] - c0[0]) / dirq, (c1[2] - c0[2]) / dirq)) if dirq else "no irqs"
            b.send("vmm accept 9000 -d &\n"); b.expect(r"\$ ", 30); sn.run(b, "sleep 1", 20)
            ma, mb = a.mark(), b.mark()
            tm = time.time()
            out = sn.run(a, "vmm migrate %s 10.7.0.2:9000" % name, 600)
            wall = time.time() - tm
            if "migrated to" not in out:
                print("%-6s %22s %22s   FAILED: %s" % (name, before, "-", out.strip()[-80:])); continue
            try:
                b.expect(r"arrived: \d+ vcpu", 180)
                b.expect(r"revision \d+ adopted \(0x[0-9a-f]+\)", 240)
            except Exception as e:
                kept = "keeping" in b.output_since(mb)
                print("%-6s %22s %22s   %s: %s" % (name, before, "-",
                      "arrived, nothing asked (the host kept its controller)" if kept else "arrived but did not adopt",
                      str(e)[:60]))
                continue
            b.send("\n"); b.expect(r"\$ ", 30)
            sa, sb = a.output_since(ma), b.output_since(mb)
            ms = S.search(sa); mr = R.search(sb)
            time.sleep(8)
            c2 = counters(info(b, name)); time.sleep(6); c3 = counters(info(b, name))
            dirq2 = c3[1] - c2[1]
            after = ("%.2f  %.1f" % ((c3[0] - c2[0]) / dirq2, (c3[2] - c2[2]) / dirq2)) if dirq2 else "no irqs"
            print("%-6s %22s %22s   %5.1f  %7s  %9s" % (
                name, before, after, wall,
                ("%.0f" % (int(ms.group(3)) / 1000)) if ms else "-",
                ("%.0f" % (int(mr.group(2)) / 1000)) if mr else "-"))
            sys.stdout.flush()
        print("four guests moved in %.0f s of wall time; the fleet's hosts are now the AIA one" % (time.time() - t0))
        # and they are all alive over there
        for name in guests:
            text = info(b, name)
            print("  %s: %s" % (name, " ".join(text.split("\n")[1].split()[:12]) if "\n" in text else "?"))
    finally:
        for q in (a, b):
            if q is not None:
                try: q.close()
                except Exception: pass
        for f in (tag + "-a.img", tag + "-b.img", link):
            try: os.unlink(os.path.join(os.getcwd(), f))
            except OSError: pass

def consolidate():
    """The consolidation, in the order a fleet meets it: a guest that needs its memory is
    refused, and the same guest once its work is done is asked, gives the memory back and
    goes. Then back to the roomy host and grown again."""
    tag = "co%d" % os.getpid()
    link = os.path.join(os.getcwd(), tag + ".sock")
    try: os.unlink(link)
    except OSError: pass
    a = th.QEMU(intc="plic", cpus=3, mem="3G", log=LOG, fsimg=tag + "-a.img",
                extra_env={"LINK": link, "LINKSRV": "on"})
    b = None
    def hostsees(q):
        return "%s MB given, %s MB resident" % resident(info(q, "lc"))
    def try_move(src, dstip, dst, t=900):
        mk = dst.mark()
        tm = time.time()
        out = sn.run(src, "vmm migrate lc %s:9000" % dstip, t)
        lines = [l.strip() for l in (src.output_since(mk) + out).splitlines()
                 if "asking the guest" in l or "could not give up" in l or "would not give up" in l
                 or "arrives at a host with" in l or "gave up" in l or "balloon" in l]
        return ("migrated to" in out), time.time() - tm, lines, out
    try:
        a.expect(r"init: starting sh", 120); a.expect(r"\$ ", 30)
        # B keeps its guests to 128 MB
        b = th.QEMU(intc="plic", cpus=3, mem="3G", log=LOG, fsimg=tag + "-b.img",
                    extra_env={"LINK": link, "LINKSRV": "off", "APPEND": "guestmem=128"})
        b.expect(r"init: starting sh", 120); b.expect(r"\$ ", 30)
        sn.run(a, "ip addr add 10.7.0.1/24 dev eth0", 30); sn.run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        a.send("vmm start -n lc -k /Image -r 0x80200000 -m 256 -a console=ttyS0 earlycon=sbi -f /linux.img -c 2 -H\n")
        a.expect(r"vm (\d+) lc: ", 30); a.expect(r"linux# ", 240)
        lx.linux_cmd(a, "dmesg | grep -c 'machrev: speaking'")
        # a program that holds 180 MB of the guest's 256: its memory is in use
        lx.linux_cmd(a, "crossck 180 600 > /tmp/ck.log 2>&1 &", 30)
        out = lx.linux_cmd(a, "for i in $(seq 90); do grep -q 'crossck:' /tmp/ck.log && break; sleep 1; done; grep MemTotal /proc/meminfo; grep MemFree /proc/meminfo", 180)
        mem = " ".join(w for w in out.split() if w.isdigit() or w == "kB")
        g.detach(a)
        print("step 1: a guest of 256 MB with 180 MB of its memory in use (MemTotal/MemFree %s)" % mem)
        print("        the host sees: " + hostsees(a))
        b.send("vmm accept 9000 -d &\n"); b.expect(r"\$ ", 30); sn.run(b, "sleep 1", 20)
        ok, secs, lines, out = try_move(a, "10.7.0.2", b)
        print("step 2: the move to a host that keeps its guests to 128 MB: %s (%.0f s)" % ("went through" if ok else "REFUSED", secs))
        for l in lines[:3]:
            print("        " + l[:160])
        if ok:
            print("        (the guest had memory to spare after all: the scenario wanted it in use)")
            # an earlier run went this way with 13 MB free before the ask: did the program that
            # held the memory survive, and what did the guest's kernel say (OOM, balloon)?
            try:
                g.attach(b, "lc"); b.send("\n"); b.expect(r"linux# ", 120)
                out = lx.linux_cmd(b, "pidof crossck || echo 'crossck: not running'; tail -3 /tmp/ck.log; "
                                      "grep -E 'MemTotal|MemFree|MemAvailable' /proc/meminfo; "
                                      "dmesg | grep -iE 'oom|killed process|out of memory|balloon|virtio_balloon' | tail -12", 120)
                print("        after the move, on the destination:")
                for l in out.splitlines():
                    if l.strip():
                        print("          " + l.strip()[:200])
                g.detach(b)
            except Exception as e:
                print("        (could not look inside the guest: %s)" % e)
            return
        # the guest is still running on A, and says so
        g.attach(a, "lc"); a.send("\n"); a.expect(r"linux# ", 120)
        out = lx.linux_cmd(a, "tail -1 /tmp/ck.log; uptime", 60)
        print("step 3: the guest kept running on the roomy host: " + " ".join(out.split()[:8]))
        # its work finishes; now the pages are free
        lx.linux_cmd(a, "kill %1 2>/dev/null; sleep 3; grep MemFree /proc/meminfo", 120)
        out = lx.linux_cmd(a, "grep MemTotal /proc/meminfo; grep MemFree /proc/meminfo", 60)
        print("step 4: its work done (MemTotal/MemFree %s)" % " ".join(w for w in out.split() if w.isdigit() or w == "kB"))
        g.detach(a)
        print("        the host sees: " + hostsees(a))
        b.send("vmm accept 9000 -d &\n"); b.expect(r"\$ ", 30); sn.run(b, "sleep 1", 20)
        ok, secs, lines, out = try_move(a, "10.7.0.2", b)
        print("step 5: the same move again: %s (%.0f s)" % ("went through" if ok else "REFUSED: " + out.strip()[-120:], secs))
        for l in lines[:3]:
            print("        " + l[:160])
        if not ok:
            return
        b.expect(r"arrived: 2 vcpu", 180)
        b.send("\n"); b.expect(r"\$ ", 30)
        print("        the host sees: " + hostsees(b))
        g.attach(b, "lc"); b.send("\n"); b.expect(r"linux# ", 120)
        out = lx.linux_cmd(b, "grep MemTotal /proc/meminfo; grep MemFree /proc/meminfo; dmesg | grep -ci 'balloon'", 60)
        print("step 6: the guest over there (MemTotal/MemFree %s)" % " ".join(w for w in out.split() if w.isdigit() or w == "kB"))
        g.detach(b)
        # and back, and grown
        a.send("vmm accept 9000 -d &\n"); a.expect(r"\$ ", 30); sn.run(a, "sleep 1", 20)
        ok, secs, lines, out = try_move(b, "10.7.0.1", a)
        print("step 7: back to the roomy host: %s" % ("went through" if ok else "REFUSED: " + out.strip()[-120:]))
        if not ok:
            return
        a.expect(r"arrived: 2 vcpu", 180)
        a.send("\n"); a.expect(r"\$ ", 30)
        m = a.mark(); a.send("vmm resize lc 384\n"); a.expect(r"\$ ", 60)
        try:
            a.consumed = m
            a.expect(r"revision \d+ adopted \(0x[0-9a-f]+\)", 180)
            a.consumed = len(a.buf)
        except Exception:
            pass
        print("        grown to 384 MB; the host sees: " + hostsees(a))
        g.attach(a, "lc"); a.send("\n"); a.expect(r"linux# ", 120)
        out = lx.linux_cmd(a, "grep MemTotal /proc/meminfo; grep MemFree /proc/meminfo; dmesg | grep 'machrev: memory' | tail -2", 60)
        # the answer, not the echo of the command: keep the lines that look like answers
        ans = [l.strip() for l in out.replace("\r", "").splitlines()
               if re.match(r"(MemTotal|MemFree|\[)", l.strip())]
        print("step 8: the guest grown again: " + " | ".join(ans)[:220])
        g.detach(a)
    finally:
        for q in (a, b):
            if q is not None:
                try: q.close()
                except Exception: pass
        for f in (tag + "-a.img", tag + "-b.img", link):
            try: os.unlink(os.path.join(os.getcwd(), f))
            except OSError: pass

if WHICH == "upgrade":
    upgrade()
elif WHICH == "consolidate":
    consolidate()
else:
    print("usage: scenario.py {upgrade,consolidate}")
