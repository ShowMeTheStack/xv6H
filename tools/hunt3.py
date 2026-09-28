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
MONA = os.path.join(os.getcwd(), tag + "-a.mon"); MONB = os.path.join(os.getcwd(), tag + "-b.mon")

def monitor(path, cmd):
    """one QEMU monitor command over its unix socket; what it printed"""
    import socket
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5); s.connect(path)
        s.recv(4096)
        s.sendall((cmd + "\n").encode())
        out = b""
        while True:
            try:
                d = s.recv(65536)
            except socket.timeout:
                break
            if not d: break
            out += d
            if out.rstrip().endswith(b"(qemu)"): break
        s.close()
        return out.decode(errors="replace")
    except Exception as e:
        return "(monitor %s: %s)" % (path, e)

def dump_harts(tag_):
    for who, path in (("A", MONA), ("B", MONB)):
        out = monitor(path, "info registers -a")
        pcs = re.findall(r"pc\s+([0-9a-f]+)", out)
        print("---- %s host %s: harts' pc: %s" % (tag_, who, " ".join(pcs)))
        for l in out.splitlines():
            if re.search(r"^\s*(pc|ra|sp|scause|sepc|stval|mcause|mepc|mstatus|sstatus|hstatus|sip|sie)\s", l):
                print("     " + l.strip()[:120])
    sys.stdout.flush()

a = th.QEMU(intc="plic", cpus=3, log=LOGA, fsimg=tag+"-a.img", extra_env={"LINK": link, "LINKSRV": "on", "MONITOR": MONA})
b = None
bad, n, lat, retries = [], 0, [], []
try:
    a.expect(r"init: starting sh", 90); a.expect(r"\$ ", 30)
    b = th.QEMU(intc="plic", cpus=3, log=LOGB, fsimg=tag+"-b.img", extra_env={"LINK": link, "LINKSRV": "off", "MONITOR": MONB})
    b.expect(r"init: starting sh", 90); b.expect(r"\$ ", 30)
    sn.run(a, "ip addr add 10.7.0.1/24 dev eth0", 30)
    sn.run(b, "ip addr add 10.7.0.2/24 dev eth0", 30)
    # the wire, both ends, headers only: what A sent and what B received when a
    # crossing stalls. Not on a long run: the capture grows without bound and
    # reading it back takes longer than the harness waits (hunt3_1000).
    if CYCLES <= 60:
        sn.run(a, "tcpdump -n -q -s 80 -w /tmp/wire.pcap port 9000 &", 20)
        sn.run(b, "tcpdump -n -q -s 80 -w /tmp/wire.pcap port 9000 &", 20)
    g.start_guest(a, "-n ax -m 32 -c 2 -a norevise", 120)
    a.send("echo gen0 > /keep; sync\n"); a.expect(r"\$ ", 30)
    g.detach(a)

    def cross(src, dst, dstip, i):
        global n
        dst.send("vmm accept 9000 -d &\n"); dst.expect(r"\$ ", 30)
        sn.run(dst, "sleep 1", 20)
        try:
            out = sn.run(src, "vmm migrate ax %s:9000" % dstip, 240)
        except Exception as e:
            bad.append("crossing %d: migrate hung: %s" % (i, str(e)[:100]))
            print("---- crossing %d: the migration hung; both hosts from the monitor ----" % i)
            dump_harts("stall"); time.sleep(2); dump_harts("2 s later")
            return False
        if "migrated to" not in out:
            # A failure the protocol handled -- the source kept its machine, the
            # destination will discard an uncommitted copy -- is a retry, counted
            # (retries[]); anything else stops the run.
            where = sn.run(src, "vmm ls", 30)
            if "running" in where and ("Input/output error" in out or "did not answer" in out):
                retries.append("crossing %d: %s" % (i, out.strip().splitlines()[-1][:100]))
                print("  crossing %d: clean failure, retrying: %s" % (i, out.strip().splitlines()[-1][:100])); sys.stdout.flush()
                try:
                    sn.run(dst, "sleep 2", 10)
                    lst = sn.run(dst, "vmm ls", 30)
                    if "ax" in lst:
                        sn.run(dst, "vmm kill ax", 60)
                except Exception:
                    pass
                return cross(src, dst, dstip, i) if len(retries) < 20 else False
            bad.append("crossing %d: migrate failed: %s" % (i, out.strip()[-160:]))
            # the wire, from both ends, as it is at the stall
            for who, h in (("src", src), ("dst", dst)):
                try:
                    print("---- %s /proc/net/tcp at the stall (crossing %d) ----" % (who, i))
                    print(sn.run(h, "cat /proc/net/tcp; ifconfig; vmm ls", 20))
                    if CYCLES <= 60:
                        print("---- %s: the last of the wire ----" % who)
                        print(sn.run(h, "tcpdump -n -t -q -r /tmp/wire.pcap port 9000 | tail -40", 120))
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
    print("crossings ok: %d; problems: %d; clean failures retried: %d" % (n, len(bad), len(retries)))
    for x in bad: print("  " + x)
    for x in retries: print("  retried " + x)
    if lat: print("sleep-1 round trip: n=%d median %.2f s max %.2f s" % (len(lat), sorted(lat)[len(lat)//2], max(lat)))
    try:
        if b: b.close()
        a.close()
    except Exception: pass
