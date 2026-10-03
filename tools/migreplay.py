#!/usr/bin/env python3
# M12, replay: a migration stream recorded once and sent again. Two xv6H hosts on the link,
# KEYED (the same migkey= on both) or not. On A, `nc' poses as a destination: it hands the source
# a nonce of its own choosing and records everything the source sends (the source then finds no
# destination's answer and keeps its guest). The recording is then sent to B's real `vmm accept'.
#   keyed:  B issues a fresh nonce, the recorded MAC was over another one: refused, authentication
#   plain:  nothing to check: B takes the recorded stream (the control: replay works without it)
# Then a real migration A -> B, to show the pair still works.
#   tools/remote.sh script mreplay tools/migreplay.py [keyed|plain] [MB]
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(HERE) == "tools":
    HERE = os.path.dirname(HERE)
os.chdir(HERE); sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("testhyp", os.path.join(HERE, "test-hyp.py"))
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
import test_hyp_guest as G
MODES = sys.argv[1].split(",") if len(sys.argv) > 1 else ["keyed", "plain"]
MB = int(sys.argv[2]) if len(sys.argv) > 2 else 16

def run(q, c, t=60):
    m = q.mark(); q.send(c + "\n"); q.expect(r"\$ ", t); return q.output_since(m)

def said(q, m):
    q.send("\n"); q.expect(r"\$ ", 60)
    return " | ".join(l.strip() for l in q.output_since(m).splitlines() if l.strip() and not l.startswith("$"))[-700:]

log = open("mreplay-console.log", "w")
for mode in MODES:
    key = "migkey=k3y-0f-the-pair" if mode == "keyed" else ""
    link = os.path.join(HERE, "mr%d.sock" % os.getpid())
    try: os.unlink(link)
    except OSError: pass
    a = T.QEMU(intc="imsic", cpus=3, log=log, fsimg="mr-a.img", extra_env={"LINK": link, "LINKSRV": "on", "APPEND": key})
    b = None
    try:
        a.expect(r"init: starting sh", 120); a.expect(r"\$ ", 30)
        b = T.QEMU(intc="imsic", cpus=3, log=log, fsimg="mr-b.img", extra_env={"LINK": link, "LINKSRV": "off", "APPEND": key})
        b.expect(r"init: starting sh", 120); b.expect(r"\$ ", 30)
        run(a, "ip addr add 10.7.0.1/24 dev eth0"); run(b, "ip addr add 10.7.0.2/24 dev eth0")
        G.start_guest(a, "-n m1 -m %d -c 1" % MB, 180)
        a.send("echo the-guest-%s > /tmp/keep\n" % mode); a.expect(r"\$ ", 30)
        G.detach(a)
        # 1. record: nc hands the source 16 bytes of the attacker's choosing as the nonce
        run(a, "echo fixed-nonce-150 > /tmp/nonce")     # 15 characters and a newline: 16 bytes
        # and the destination's "accept" (MIG_HS_ACCEPT 'A' and four bytes of argument, ignored)
        run(a, "echo AAAA >> /tmp/nonce")
        run(a, "nc -l -w 8 9100 < /tmp/nonce > /tmp/rec &")
        time.sleep(1)
        out = run(a, "vmm migrate m1 10.7.0.1:9100", 600)
        time.sleep(10)
        sz = run(a, "ls -l /tmp/rec", 30)
        print("REPLAY %s record: source says %s; recorded %s" % (mode,
              " | ".join(l.strip() for l in out.splitlines() if "vmm" in l or "hyp" in l or "migrat" in l)[-300:],
              " ".join(l.strip() for l in sz.splitlines() if "rec" in l and "ls" not in l)))
        G.attach(a, "m1"); a.send("\n"); a.expect(r"\$ ", 60)
        o = run(a, "cat /tmp/keep", 30)
        print("  the guest after the recording, on A: %s" % ("ok" if "the-guest-%s" % mode in o else "MISSING " + o[-200:]))
        G.detach(a)
        # 2. replay the recording to B's real destination
        run(b, "vmm accept 9000 -d &"); time.sleep(1)
        mb = b.mark()
        run(a, "nc -w 8 10.7.0.2 9000 < /tmp/rec > /tmp/got", 300)
        time.sleep(3)
        bs = said(b, mb)
        ls = run(b, "vmm list", 30)
        print("REPLAY %s replayed to B: B said: %s" % (mode, bs))
        print("  B's machines after the replay: %s" % " | ".join(l.strip() for l in ls.splitlines()[1:] if l.strip() and not l.startswith("$"))[:300])
        sys.stdout.flush()
        # 3. a real migration, to show the pair works
        run(b, "vmm accept 9001 -d &"); time.sleep(1)
        out = run(a, "vmm migrate m1 10.7.0.2:9001", 600)
        moved = "migrated to" in out
        if moved:
            G.attach(b, "m1"); b.send("\n"); b.expect(r"\$ ", 60)
            o = run(b, "cat /tmp/keep", 30); G.detach(b)
        print("REPLAY %s real migration A->B: %s%s" % (mode, "moved" if moved else "NOT moved",
              ("; the guest on B: " + ("ok" if "the-guest-%s" % mode in o else "MISSING")) if moved else ""))
        sys.stdout.flush()
    finally:
        for q in (a, b):
            if q:
                q.close()
        for f in (link, "mr-a.img", "mr-b.img"):
            try: os.unlink(f)
            except OSError: pass
