#!/usr/bin/env python3
# E5/M11, crossings: what a service sees when its machine moves to another host, of the same
# shape or of another (revised on arrival). Two xv6H hosts on QEMU joined by a private link
# (10.7.0.1, 10.7.0.2); a Linux guest (2 vcpus, its disk, 10.7.0.50 on the link, port 6379
# published) runs `kvlat serve'; host A runs `kvlat load' at 500 requests a second against it.
# The guest keeps its address and its TCP state across the move (-k 2000: a connection silent
# for two seconds with requests waiting is given up and made again).
# The guest crosses A -> B and back ROUNDS times; B differs from A by AXIS (a boot word of
# test_hyp_snap.LX_AXES, or "same"). Per crossing: the gap (the longest time without an answer),
# errors (requests lost with the old connection), the worst bin, and the arrival's adoption.
#   tools/remote.sh script tmig tools/taillat_mig.py AXIS [ROUNDS]
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.basename(HERE) == "tools":
    HERE = os.path.dirname(HERE)
os.chdir(HERE); sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("testhyp", os.path.join(HERE, "test-hyp.py"))
T = importlib.util.module_from_spec(spec); spec.loader.exec_module(T)
import test_hyp_guest as G, test_hyp_linux as L, test_hyp_snap as S
AXIS = sys.argv[1] if len(sys.argv) > 1 else "same"
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 3
append = "" if AXIS == "same" else S.LX_AXES[AXIS][0]
link = os.path.join(HERE, "tmig%d.sock" % os.getpid())
try: os.unlink(link)
except OSError: pass
log = open("tmig-console.log", "w")
a = T.QEMU(intc="imsic", cpus=3, log=log, fsimg="tmig-a.img", extra_env={"LINK": link, "LINKSRV": "on"})
b = None
rows = []
said = set()
def run(q, c, t=60):
    m = q.mark(); q.send(c + "\n"); q.expect(r"\$ ", t); return q.output_since(m)
def client_tcp():
    # the client's recovery so far (xv6H's TCP, /proc/net/sockstat's TCPX line)
    m = re.search(r"TCPX: rto (\d+) maxrto_ms (\d+) fast (\d+) partial (\d+)", run(a, "cat /proc/net/sockstat", 30))
    return tuple(int(x) for x in m.groups()) if m else None
def host_vnet(q):
    # a host's guest-NIC drops by reason and its frames for a guest not running there
    m = re.search(r"VNETX: drop_stopped (\d+) drop_nobuf (\d+) drop_other (\d+) orphan_in (\d+) orphan_out (\d+)",
                  run(q, "cat /proc/net/sockstat", 30))
    return tuple(int(x) for x in m.groups()) if m else None
def vdelta(x, y):
    if not x or not y:
        return "-"
    return "stopped +%d nobuf +%d other +%d orphan_in +%d orphan_out +%d" % tuple(b - a for a, b in zip(x, y))
def guest_tcp(q):
    # the guest's (Linux): RetransSegs, and TCPTimeouts, TCPLossProbes, TCPSpuriousRTOs, TCPFastRetrans
    G.attach(q, "lm"); q.send("\n"); q.expect(r"linux# ", 120)
    o = L.linux_cmd(q, "grep Tcp: /proc/net/snmp; grep TcpExt: /proc/net/netstat", 60)
    G.detach(q)
    v = {}
    for hdr, val in ((l1, l2) for l1, l2 in zip(o.splitlines(), o.splitlines()[1:])
                     if l1.split(":")[0] == l2.split(":")[0] and not re.search(r"\d", l1.split(":", 1)[1][:20])):
        for k, x in zip(hdr.split()[1:], val.split()[1:]):
            if x.lstrip("-").isdigit():
                v[k] = int(x)
    return {k: v.get(k) for k in ("RetransSegs", "TCPTimeouts", "TCPLossProbes", "TCPSpuriousRTOs", "TCPFastRetrans")}
def delta(x, y):
    if not x or not y:
        return "-"
    if isinstance(x, dict):
        return " ".join("%s +%s" % (k, (y[k] - x[k]) if x[k] is not None and y[k] is not None else "?") for k in x)
    return "rto +%d (max armed %d ms) fast +%d partial +%d" % (y[0] - x[0], y[1], y[2] - x[2], y[3] - x[3])
try:
    a.expect(r"init: starting sh", 120); a.expect(r"\$ ", 30)
    b = T.QEMU(intc="imsic", cpus=3, log=log, fsimg="tmig-b.img", extra_env={"LINK": link, "LINKSRV": "off", "APPEND": append})
    b.expect(r"init: starting sh", 120); b.expect(r"\$ ", 30)
    run(a, "ip addr add 10.7.0.1/24 dev eth0"); run(b, "ip addr add 10.7.0.2/24 dev eth0")
    L.LINUX_ARGS = L.LINUX_ARGS.replace("-m 256", "-m 128")
    # the guest's address is on the link both hosts share: it keeps it across the move, and its
    # connections with it (the client needs one address)
    vmid = L.start_linux(a, "-f /linux.img -c 2 -H -V -i 10.7.0.50/24 -p 6379", name="lm", timeout=300)
    L.linux_cmd(a, "mount /dev/vda /mnt; setsid kvlat serve < /dev/null > /tmp/kvs.log 2>&1 &", 60)
    # the guest's own view of a stall: when eth0's received count stands still for 0.6 s, the
    # interrupt counts go to the kernel log (read back after a crossing that lost requests)
    # (one printf: linux_cmd appends a marker to the line, which a heredoc's terminator cannot take)
    L.linux_cmd(a, 'printf \'%s\\n\' \'p=x; t=0\' \'while sleep 0.2; do\' \'  r=$(cat /sys/class/net/eth0/statistics/rx_packets)\' \'  if [ "$r" = "$p" ]; then t=$((t+1)); else t=0; fi\' \'  if [ $t = 3 ] || [ $t = 12 ]; then echo "GMON stall $t rx $r" > /dev/kmsg\' \'    grep -v CPU /proc/interrupts | while read l; do echo "GMON $l" > /dev/kmsg; done; fi\' \'  p=$r\' \'done\' > /tmp/gmon.sh', 30)
    L.linux_cmd(a, "setsid sh /tmp/gmon.sh < /dev/null > /dev/null 2>&1 &", 30)
    G.detach(a)
    SOFT = os.environ.get("SOFTIMSIC") or (len(sys.argv) > 3 and sys.argv[3] == "soft")   # (a unit gets no environment)
    if SOFT:
        # the software IMSIC model, whose claims the host sees (E5's lost interrupts)
        print("  software IMSIC: %s" % run(a, "vmm imsichw lm off", 30).strip().splitlines()[-1:]); sys.stdout.flush()
    here, there = a, b
    for k in range(2 * ROUNDS):
        # (the guest's own TCP counters were 0 in 49 of 50 crossings; reading them costs two attaches)
        GT = os.environ.get("GUESTTCP")
        c0 = client_tcp(); g0 = guest_tcp(here) if GT else None; va0 = host_vnet(a); vb0 = host_vnet(b)
        # the load, from A, for 40 s; the crossing 5 s in
        run(a, "kvlat load -c 10.7.0.50 -r 500 -t 40 -b 100 -k 2000 > /tmp/kv.out &")
        time.sleep(4)
        # the destination listens only for this move (a listener left waiting on the client's host
        # through the measurement coincided with the two runs that lost the service early)
        run(there, "vmm accept %s -d &" % ("9000" if there is b else "9001"))
        time.sleep(1)
        dest = "10.7.0.2:9000" if here is a else "10.7.0.1:9001"
        m = here.mark()
        t_ask = time.time()
        out = run(here, "vmm migrate lm %s" % dest, 600)
        ok = "migrated to" in out
        print("  the migration command took %.2f s (%s)" % (time.time() - t_ask, " | ".join(l for l in out.splitlines() if "crossing" in l or "pre-copy" in l or "stopped" in l)[:400])); sys.stdout.flush()
        try:
            mm = there.expect(r"revision \d+ adopted \(0x[0-9a-f]+\) after (\d+) us", 30)
            adopt = int(mm.group(1)) / 1000.0
        except Exception:
            adopt = None
        there.send("\n"); there.expect(r"\$ ", 60)
        if SOFT:
            run(there, "vmm imsichw lm off", 30)
        t_mig = time.time()
        # the network's view, without TCP's back-off: A pings the guest until it answers
        first = None
        for j in range(120):
            o = run(a, "ping 10.7.0.50 1", 30)
            if re.search(r"1 received", o):
                first = time.time() - t_mig; break
            if j < 12:
                # and what the guest is doing meanwhile, from the host it is on
                s = run(there, "vmm info lm | grep -E 'pc 0x|exits|clock:'", 30)
                print("  SAMPLE %.1f s: %s" % (time.time() - t_mig, " | ".join(l.strip() for l in s.splitlines() if "pc 0x" in l or "cause" in l or "clock" in l)[:600])); sys.stdout.flush()
        print("  ping: the guest answered %s after the move's end" % ("%.2f s" % first if first is not None else "never (120 tries)")); sys.stdout.flush()
        time.sleep(40 - 5 + 8)
        o = run(a, "cat /tmp/kv.out", 60)
        bad = [l for l in o.splitlines() if l.startswith("kvlat bin") and (" err 0" not in l or re.search(r"p99 (\d+)", l) and int(re.search(r"p99 (\d+)", l).group(1)) > 100000)]
        print("  BINS (the move asked at 5.0 s; %d bins affected): %s" % (len(bad), " ; ".join(b.replace("kvlat bin ", "") for b in bad)[:1500])); sys.stdout.flush()
        c1 = client_tcp(); g1 = guest_tcp(there) if GT else None; va1 = host_vnet(a); vb1 = host_vnet(b)
        print("  TCP during the round: client %s; guest %s" % (delta(c0, c1), delta(g0, g1)))
        print("  NIC during the round: A %s; B %s" % (vdelta(va0, va1), vdelta(vb0, vb1))); sys.stdout.flush()
        for h, nm in ((a, "A"), (b, "B")):
            for l in re.findall(r"(?:hyp: vm \d+: nic: (?:no receive buffer|interrupt raised|msi -)|sched: )[^\n]*", h.output_since(0)[-200000:]):
                if l not in said:
                    said.add(l); print("  %s: %s" % (nm, l))
        t = re.search(r"kvlat total (\d+) answered (\d+) errors (\d+) reconnects (\d+) p50 (\d+) p99 (\d+) p999 (\d+) max (\d+) gap (\d+)", o)
        row = (k, "A->B" if here is a else "B->A", ok, t.groups() if t else None, adopt)
        rows.append(row)
        if t and t.group(3) != "0":
            # the guest's own record of the stall
            G.attach(there, "lm"); there.send("\n"); there.expect(r"linux# ", 120)
            g = L.linux_cmd(there, "dmesg | grep -E \"GMON (stall|14:|IPI)\" | tail -40; dmesg -c > /dev/null", 120)
            G.detach(there)
            print("  GUEST: " + " | ".join(l.strip() for l in g.splitlines() if "GMON" in l)[-3000:])
        print("E5MIG %s %d %s migrated %s: %s; adoption on arrival %s ms" % (AXIS, k, row[1], ok,
              t.group(0) if t else "(no total)", "%.1f" % adopt if adopt else "-")); sys.stdout.flush()
        here, there = there, here
    if here is b:      # leave the guest on A
        pass
finally:
    for q in (a, b):
        if q:
            q.close()
    for f in (link, "tmig-a.img", "tmig-b.img"):
        try: os.unlink(f)
        except OSError: pass
print("\n| axis | crossing | answered | errors | gap (ms) | p99 (ms) | adoption (ms) |")
print("|---|---|---|---|---|---|---|")
for k, d, ok, t, ad in rows:
    if t:
        print("| %s | %s | %s/%s | %s | %.0f | %.1f | %s |" % (AXIS, d, t[1], t[0], t[2], int(t[8]) / 1000.0, int(t[5]) / 1000.0,
              "%.1f" % ad if ad else "-"))
