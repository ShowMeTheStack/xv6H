#!/usr/bin/env python3
# stockcross: the crossing of a stock Linux, line by line. A stock Linux 6.12
# (no module, no patches: LXTAG=-stock) on host A crosses to host B, whose controller is the
# other kind, and is served there by B's model of the controller it left with. DIR plic2imsic
# (the default) or imsic2plic (an AIA guest on a host with no AIA). Prints the move, B's
# lines about the arrival, the guest's own word that it is alive, its disk after the move, and
# its /proc/interrupts on B. ROUNDS crossings, each on
# a fresh pair of hosts.
#   LXTAG=-stock tools/remote.sh script NAME tools/stockcross.py [rounds] [plic2imsic|imsic2plic]
import os, re, sys
os.chdir(os.path.dirname(os.path.abspath(__file__)))   # run as run_script.py at the tree's root
from importlib.machinery import SourceFileLoader
th = SourceFileLoader("th", "test-hyp.py").load_module()
g = SourceFileLoader("g", "test_hyp_guest.py").load_module()
lx = SourceFileLoader("lx", "test_hyp_linux.py").load_module()
sn = SourceFileLoader("sn", "test_hyp_snap.py").load_module()
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 1
DIR = sys.argv[2] if len(sys.argv) > 2 else "plic2imsic"
IA, IB = ("imsic", "plic") if DIR == "imsic2plic" else ("plic", "imsic")
LOG = open("console.log", "w")

for r in range(ROUNDS):
    tag = "sc%d_%d" % (os.getpid(), r)
    link = os.path.join(os.getcwd(), tag + ".sock")
    a = b = None
    try:
        a = th.QEMU(intc=IA, cpus=3, log=LOG, fsimg=tag + "-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
        a.expect(r"init: starting sh", 90); a.expect(r"\$ ", 30)
        b = th.QEMU(intc=IB, cpus=3, log=LOG, fsimg=tag + "-b.img", extra_env={"LINK": link, "LINKSRV": "off"})
        b.expect(r"init: starting sh", 90); b.expect(r"\$ ", 30)
        sn.run(a, "ip addr add 10.7.0.1/24 dev eth0", 30); sn.run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
        a.send("vmm start -n lx -k /Image -r 0x80200000 -m 256 -a console=ttyS0 earlycon=sbi -f /linux.img -c 1 -H\n")
        a.expect(r"vm (\d+) lx: ", 20); a.expect(r"linux# ", 120)
        out = lx.linux_cmd(a, "uname -r; lsmod 2>/dev/null | grep -c machrev; grep -o 'smaia[a-z_]*' /proc/cpuinfo | head -1; "
                              "mount /dev/vda /mnt && echo before-the-move > /mnt/stock && sync && echo written", 60)
        print("round %d, on A (%s): %s" % (r, IA, " | ".join(l.strip() for l in out.splitlines() if l.strip())[:200]))
        g.detach(a)
        mk = b.mark()
        b.send("vmm accept 9000 -d &\n"); b.expect(r"\$ ", 30); sn.wait_listening(b, mk)
        out = sn.run(a, "vmm migrate lx 10.7.0.2:9000", 600)
        print("  migrate: " + " | ".join(l.strip() for l in out.splitlines() if "migrat" in l or "pre-copy" in l or "stopped for" in l)[:300])
        b.expect(r"arrived: 1 vcpu", 120)
        arr = [l.strip() for l in b.output_since(mk).splitlines() if "hyp:" in l or "arrived" in l]
        for l in arr:
            print("  B: " + l[:200])
        g.attach(b, "lx"); b.send("\n"); b.expect(r"linux# ", 120)
        out = lx.linux_cmd(b, "echo LINUX-ON-B-ALIVE; cat /mnt/stock; echo after-the-move >> /mnt/stock; sync; tail -1 /mnt/stock; "
                              "cat /proc/interrupts", 60)
        print("  on B:")
        for l in out.splitlines():
            if l.strip():
                print("    " + l.rstrip()[:160])
        sys.stdout.flush()
    finally:
        for q in (a, b):
            if q:
                q.close()
        try: os.unlink(link)
        except OSError: pass
