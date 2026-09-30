#!/usr/bin/env python3
# scale (L14): the mechanism's cost along an axis, each a curve for the paper's
# scaling subsection. Adoption latency is the host's "revision G adopted (0xM) after
# T us" line, N revisions there and N back per point; the same measure as
# adoptprobe.py, on one axis at a time.
#
#   python3 tools/scale.py vcpus xv6|linux [N]   (a) 1, 2, 4, 8 vcpus; a device fact
#                                                (rtc), a hart fact (hartid), the cache
#                                                block (cboz), and for xv6 the controller
#   python3 tools/scale.py ram xv6|linux [N]     (c) F13: +64, +256, +1024, +2048 MB
#                                                added to a running guest (vmm resize)
#   python3 tools/scale.py load xv6 [N]          (e) 1, 2, 3 guests on the host, every
#                                                other one busy; the first one revised
#   python3 tools/scale.py devices xv6 [N]       (b) F15 with 3 or 4 virtio devices in the tree
#   python3 tools/scale.py console xv6 [N]       the guest's console line: UART vs virtio
import os, re, sys, time, statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
lx = SourceFileLoader("lx", "test_hyp_linux.py").load_module()
axis = sys.argv[1] if len(sys.argv) > 1 else "vcpus"
kind = sys.argv[2] if len(sys.argv) > 2 else "xv6"
N = int(sys.argv[3]) if len(sys.argv) > 3 else 20
LOG = open("console.log", "w")

def host(q, cmd, t=60):
    m = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", t); return q.output_since(m)

def adopted(q, vmid, cmd, t=60):
    """send a host command that asks a revision; return the adoption latency in ms."""
    mk = q.mark(); q.send(cmd + "\n"); q.expect(r"\$ ", 30)
    q.consumed = mk
    mm = q.expect(r"hyp: vm %d: revision \d+ adopted \(0x[0-9a-f]+\) after (\d+) us" % vmid, t)
    q.consumed = len(q.buf)
    return int(mm.group(1)) / 1000.0

def measure(q, vmid, there, back, n, settle=None):
    lat, miss = [], 0
    for i in range(n):
        for words in (there, back):
            try:
                lat.append(adopted(q, vmid, "vmm revise %d %s" % (vmid, words)))
            except Exception:
                miss += 1
                q.send("\n"); q.expect(r"\$ ", 60)
            if settle:
                host(q, settle, 30)
    return lat, miss

def row(label, fact, lat, miss):
    if lat:
        s = sorted(lat)
        print("%-28s %-8s n=%-3d miss=%-2d median %7.1f  p90 %7.1f  max %7.1f ms" % (
            label, fact, len(lat), miss, statistics.median(s), s[int(0.9 * (len(s) - 1))], s[-1]))
    else:
        print("%-28s %-8s n=0 miss=%d" % (label, fact, miss))
    sys.stdout.flush()

def start(q, args):
    if kind == "xv6":
        return g.start_guest(q, args, 180)
    return lx.start_linux(q, "-f none -H " + args, timeout=240)

FACTS = [("rtc", "rtc off", "rtc on"), ("hartid", "hartid 4", "hartid 0"), ("cboz", "cboz 128", "cboz 64")]
if kind == "xv6":
    FACTS = [("intc", "plic", "imsic")] + FACTS

if axis == "vcpus":
    for v in (1, 2, 4, 8):
        q = th.QEMU(intc="imsic" if kind == "xv6" else "plic", cpus=max(3, v), mem="2G", log=LOG)
        try:
            q.expect(r"init: starting sh", 120); q.expect(r"\$ ", 30)
            vmid = start(q, "-c %d -m %d" % (v, 64 if kind == "xv6" else 256))
            g.detach(q)
            for fact, there, back in FACTS:
                lat, miss = measure(q, vmid, there, back, N)
                row("vcpus %d (host %d harts)" % (v, max(3, v)), fact, lat, miss)
        finally:
            q.close()

elif axis == "ram":
    # a fresh guest per point: F13 only grows, and N additions of 2 GB would carry the
    # guest's memory node far past the host's (the first run reached 34 GB, unbacked)
    base = 64 if kind == "xv6" else 256
    for add in (64, 256, 1024, 2048):
        q = th.QEMU(intc="imsic" if kind == "xv6" else "plic", cpus=3, mem="8G", log=LOG)
        try:
            q.expect(r"init: starting sh", 120); q.expect(r"\$ ", 30)
            vmid = start(q, "-c 2 -m %d" % base)
            g.detach(q)
            size = base
            lat, miss = [], 0
            for i in range(N):
                size += add
                try:
                    lat.append(adopted(q, vmid, "vmm resize %d %d" % (vmid, size), 120))
                except Exception:
                    miss += 1; q.send("\n"); q.expect(r"\$ ", 60)
            row("ram +%d MB per revision (to %d MB)" % (add, size), "F13", lat, miss)
        finally:
            q.close()

elif axis == "load":
    # the host image carries three guest disks (GUESTDISKS=3): guest 1 has /disk0.img, so
    # up to two busy guests beside it; the host has three harts, so three guests already
    # time-slice them
    for k in (1, 2, 3):
        q = th.QEMU(intc="imsic", cpus=3, mem="4G", log=LOG)
        try:
            q.expect(r"init: starting sh", 120); q.expect(r"\$ ", 30)
            vmid = g.start_guest(q, "-c 1 -m 32", 180)
            g.detach(q)
            for j in range(k - 1):
                other = g.start_guest(q, "-n b%d -c 1 -m 32 -f /disk%d.img" % (j, j + 1), 180)   # its own disk: /disk0.img is guest 1's
                # busy: files written and read back, processes forked and switched, for the whole run
                q.send("fsbench -s 4 -n 64 -q > /tmp/fs.out 2>&1 &\n"); q.expect(r"\$ ", 30)
                q.send("schedbench -t 3000 -c -1 > /tmp/sched.out 2>&1 &\n"); q.expect(r"\$ ", 30)
                g.detach(q)
            time.sleep(3)
            for fact, there, back in FACTS:
                lat, miss = measure(q, vmid, there, back, N)
                row("%d guest(s), %d busy" % (k, k - 1), fact, lat, miss)
        finally:
            q.close()

elif axis == "console":
    # is the xv6 guest's 5-10 ms its own console line? The handler prints one line
    # before it acknowledges; through the 16550 model that is an exit per character.
    # The same facts with the UART console, then with a virtio console, then back.
    q = th.QEMU(intc="imsic", cpus=3, mem="2G", log=LOG)
    try:
        q.expect(r"init: starting sh", 120); q.expect(r"\$ ", 30)
        vmid = g.start_guest(q, "-c 2 -m 64", 180)
        g.detach(q)
        for con in ("uart", "virtio", "uart"):
            if con != "uart" or True:
                try: adopted(q, vmid, "vmm revise %d console %s" % (vmid, con))
                except Exception: q.send("\n"); q.expect(r"\$ ", 60)
            for fact, there, back in FACTS:
                lat, miss = measure(q, vmid, there, back, N)
                row("console %s" % con, fact, lat, miss)
    finally:
        q.close()

elif axis == "devices":
    # the map revision moves every virtio window at once (F15): the disk, the NIC, the
    # balloon and, when asked, a virtio console: 3 or 4 devices to rebase
    q = th.QEMU(intc="imsic", cpus=3, mem="2G", log=LOG)
    try:
        q.expect(r"init: starting sh", 120); q.expect(r"\$ ", 30)
        # a guest without its disk has no root and no shell, so the smallest tree is the
        # default one: disk, NIC and balloon (3); a virtio console makes 4
        for ndev, args, extra in ((3, "-c 2 -m 64", None), (4, "-c 2 -m 64", "console virtio")):
            try:
                vmid = g.start_guest(q, args, 180)
            except Exception as e:
                print("devices %d: could not start with %r (%s)" % (ndev, args, str(e)[:80])); continue
            g.detach(q)
            if extra:
                try: adopted(q, vmid, "vmm revise %d %s" % (vmid, extra))
                except Exception: q.send("\n"); q.expect(r"\$ ", 60)
            lat, miss = measure(q, vmid, "map alt", "map default", N)
            row("%d virtio device(s)" % ndev, "F15 map", lat, miss)
            host(q, "vmm kill %d" % vmid, 60)
    finally:
        q.close()
